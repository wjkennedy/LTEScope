#include "sdr/esp_sdr_source.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <utility>

#if !defined(__EMSCRIPTEN__) && !defined(_WIN32)
#include <cerrno>
#include <fcntl.h>
#include <glob.h>
#include <poll.h>
#include <termios.h>
#include <unistd.h>
#endif

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>

namespace {
EspSdrSource* g_browser_esp_source = nullptr;
}

extern "C" {
EMSCRIPTEN_KEEPALIVE void cellscope_web_esp_prepare_result(
    int ok, const char* msg, double actualRate, double actualCenter,
    double minFreq, double maxFreq, int maxGainIndex, int continuous)
{
    if (g_browser_esp_source)
        g_browser_esp_source->onBrowserPrepareResult(
            ok != 0, msg ? std::string(msg) : std::string(), actualRate,
            actualCenter, minFreq, maxFreq, maxGainIndex, continuous != 0);
}

EMSCRIPTEN_KEEPALIVE void cellscope_web_esp_sample_block(
    const unsigned char* data, int len, int bits, int fullScale)
{
    if (g_browser_esp_source && data && len > 0)
        g_browser_esp_source->onSamples(data, len, bits, fullScale);
}
}

EM_JS(void, web_esp_configure,
      (double centerFreq, double sampleRate, double gainIndex, double ppm), {
    Module.cellscopeEsp.configure({centerFreq, sampleRate, gainIndex, ppm});
});
EM_JS(void, web_esp_begin_prepare, (), { Module.cellscopeEsp.beginPrepare(); });
EM_JS(void, web_esp_start_stream, (), { Module.cellscopeEsp.startStream(); });
EM_JS(void, web_esp_stop, (), { Module.cellscopeEsp.stop(); });
EM_JS(void, web_esp_set_center_freq, (double hz), { Module.cellscopeEsp.setCenterFreq(hz); });
EM_JS(void, web_esp_set_sample_rate, (double hz), { Module.cellscopeEsp.setSampleRate(hz); });
EM_JS(void, web_esp_set_gain, (double gainIndex), { Module.cellscopeEsp.setGain(gainIndex); });
EM_JS(void, web_esp_set_ppm, (double ppm), { Module.cellscopeEsp.setPpm(ppm); });

EspSdrSource::~EspSdrSource() { stop(); }

std::vector<SdrDeviceInfo> EspSdrSource::listDevices()
{
    return {{0, "ESP-SDR board (Web Serial)", "connect on Start"}};
}

void EspSdrSource::setCenterFreq(double hz)
{
    centerFreq_ = hz;
    if (browserPrepared_ || running_.load()) web_esp_set_center_freq(hz);
}

void EspSdrSource::setSampleRate(double hz)
{
    sampleRate_ = hz;
    dcRate_ = (float)(50.0 / std::max(1.0, sampleRate_));
    if (browserPrepared_ || running_.load()) web_esp_set_sample_rate(hz);
}

void EspSdrSource::setGain(double gainIndex)
{
    gainIndex_ = gainIndex;
    if (browserPrepared_ || running_.load()) web_esp_set_gain(gainIndex);
}

void EspSdrSource::setPpm(double ppm)
{
    ppm_ = ppm;
    if (browserPrepared_ || running_.load()) web_esp_set_ppm(ppm);
}

bool EspSdrSource::beginBrowserPrepare(int, BrowserPrepareCb cb, std::string& err)
{
    if (browserPreparing_)
    {
        err = "ESP-SDR Web Serial prepare already in progress";
        return false;
    }
    browserPrepareCb_ = std::move(cb);
    browserPrepared_ = false;
    browserPreparing_ = true;
    g_browser_esp_source = this;
    web_esp_configure(centerFreq_, sampleRate_, gainIndex_, ppm_);
    web_esp_begin_prepare();
    return true;
}

void EspSdrSource::onBrowserPrepareResult(bool ok, const std::string& msg,
                                          double actualRate, double actualCenter,
                                          double minFreq, double maxFreq,
                                          int maxGainIndex, bool continuous)
{
    browserPreparing_ = false;
    browserPrepared_ = ok;
    if (ok)
    {
        if (actualRate > 0.0) sampleRate_ = actualRate;
        if (actualCenter > 0.0) centerFreq_ = actualCenter;
        minFreq_ = minFreq;
        maxFreq_ = maxFreq;
        maxGainIndex_ = maxGainIndex;
        continuous_ = continuous;
        firmwareName_ = msg;
    }
    if (browserPrepareCb_)
        browserPrepareCb_(ok, ok ? std::string() : msg);
    if (!ok)
    {
        browserPrepareCb_ = nullptr;
        running_.store(false);
    }
}

bool EspSdrSource::start(int, SdrSampleCb cb, std::string& err)
{
    if (running_.load()) return true;
    if (!browserPrepared_)
    {
        err = "ESP-SDR Web Serial device is not prepared";
        return false;
    }
    cb_ = std::move(cb);
    dcOffRe_ = dcOffIm_ = 0.0f;
    running_.store(true);
    web_esp_start_stream();
    return true;
}

void EspSdrSource::stop()
{
    running_.store(false);
    browserPreparing_ = false;
    browserPrepared_ = false;
    browserPrepareCb_ = nullptr;
    web_esp_stop();
    if (g_browser_esp_source == this) g_browser_esp_source = nullptr;
    cb_ = nullptr;
}

void EspSdrSource::onSamples(const unsigned char* data, int len, int bits, int fullScale)
{
    processSamples(data, len, bits, fullScale);
}

#elif !defined(_WIN32)

namespace {
bool writeAll(int fd, const void* data, size_t size)
{
    const auto* p = static_cast<const unsigned char*>(data);
    while (size > 0) {
        const ssize_t n = ::write(fd, p, size);
        if (n > 0) { p += n; size -= (size_t)n; continue; }
        if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        return false;
    }
    return true;
}

bool readExact(int fd, void* data, size_t size, const std::atomic<bool>* running,
               int timeoutMs = 5000)
{
    auto* p = static_cast<unsigned char*>(data);
    while (size > 0 && (!running || running->load())) {
        pollfd item{fd, POLLIN, 0};
        const int ready = ::poll(&item, 1, 100);
        if (ready < 0 && errno == EINTR) continue;
        if (ready < 0) return false;
        if (ready == 0) { timeoutMs -= 100; if (timeoutMs <= 0) return false; continue; }
        const ssize_t n = ::read(fd, p, size);
        if (n > 0) { p += n; size -= (size_t)n; continue; }
        if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        return false;
    }
    return size == 0;
}

bool readLine(int fd, std::string& line, const std::atomic<bool>* running = nullptr)
{
    line.clear();
    char c = 0;
    while (line.size() < 512) {
        if (!readExact(fd, &c, 1, running)) return false;
        if (c == '\n') { if (!line.empty() && line.back() == '\r') line.pop_back(); return true; }
        line.push_back(c);
    }
    return false;
}

bool command(int fd, const std::string& text, std::string& result,
             const char* prefix = nullptr)
{
    const std::string wire = text + "\n";
    if (!writeAll(fd, wire.data(), wire.size())) return false;
    for (int i = 0; i < 80; ++i) {
        if (!readLine(fd, result)) return false;
        if (result.rfind("ERR ", 0) == 0) return false;
        if (!prefix || result.rfind(prefix, 0) == 0) return true;
    }
    return false;
}

int nearestDecimation(double rate)
{
    const int choices[] = {8, 32, 64, 128, 256, 512, 1024};
    int best = choices[0];
    for (int value : choices)
        if (std::abs(16000000.0 / value - rate) < std::abs(16000000.0 / best - rate)) best = value;
    return best;
}
}

EspSdrSource::~EspSdrSource() { stop(); }

std::vector<SdrDeviceInfo> EspSdrSource::listDevices()
{
    nativePaths_.clear();
    const char* patterns[] = {
        "/dev/cu.usbmodem*", "/dev/cu.usbserial*", "/dev/cu.SLAB_USBtoUART*",
        "/dev/cu.wchusbserial*", "/dev/cu.wchusbmodem*", "/dev/ttyACM*", "/dev/ttyUSB*"
    };
    for (const char* pattern : patterns) {
        glob_t matches{};
        if (::glob(pattern, 0, nullptr, &matches) == 0)
            for (size_t i = 0; i < matches.gl_pathc; ++i) {
                const std::string path = matches.gl_pathv[i];
                if (std::find(nativePaths_.begin(), nativePaths_.end(), path) == nativePaths_.end())
                    nativePaths_.push_back(path);
            }
        ::globfree(&matches);
    }
    std::vector<SdrDeviceInfo> result;
    for (size_t i = 0; i < nativePaths_.size(); ++i)
        result.push_back({(int)i, "ESP-SDR serial", nativePaths_[i]});
    return result;
}

void EspSdrSource::setCenterFreq(double hz)
{
    centerFreq_ = hz;
    if (serialFd_ >= 0 && running_.load()) nativeRestartStream();
}
void EspSdrSource::setSampleRate(double hz)
{
    sampleRate_ = hz;
    dcRate_ = (float)(50.0 / std::max(1.0, sampleRate_));
    if (serialFd_ >= 0 && running_.load()) nativeRestartStream();
}
void EspSdrSource::setGain(double gainIndex)
{
    gainIndex_ = gainIndex;
    if (serialFd_ >= 0 && running_.load()) nativeRestartStream();
}
void EspSdrSource::setPpm(double ppm)
{
    ppm_ = ppm;
    if (serialFd_ >= 0 && running_.load()) nativeRestartStream();
}

bool EspSdrSource::start(int deviceIndex, SdrSampleCb cb, std::string& err)
{
    if (running_.load()) return true;
    if (nativePaths_.empty()) listDevices();
    if (deviceIndex < 0 || deviceIndex >= (int)nativePaths_.size()) {
        err = "Select an ESP-SDR serial device";
        return false;
    }
    serialFd_ = ::open(nativePaths_[(size_t)deviceIndex].c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (serialFd_ < 0) { err = "Cannot open " + nativePaths_[(size_t)deviceIndex] + ": " + std::strerror(errno); return false; }
    termios tty{};
    if (::tcgetattr(serialFd_, &tty) != 0) { err = "Cannot configure serial device"; ::close(serialFd_); serialFd_ = -1; return false; }
    ::cfmakeraw(&tty);
    ::cfsetspeed(&tty, B115200); // Native USB ignores baud; safe for UART bridges.
    tty.c_cc[VMIN] = 0; tty.c_cc[VTIME] = 1;
    ::tcsetattr(serialFd_, TCSANOW, &tty);
    ::tcflush(serialFd_, TCIOFLUSH);

    std::string line;
    const std::string nonce = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    if (!command(serialFd_, "SYNC " + nonce, line, ("SYNC " + nonce).c_str()) ||
        !command(serialFd_, "INFO", firmwareName_) ||
        (firmwareName_.rfind("S3SDR 6", 0) != 0 && firmwareName_.rfind("ESP32SDR 6", 0) != 0)) {
        err = "ESP-SDR protocol 6 handshake failed"; ::close(serialFd_); serialFd_ = -1; return false;
    }
    std::string transport;
    if (!command(serialFd_, "TRANSPORT?", transport, "TRANSPORT ")) transport = "TRANSPORT UART";
    continuous_ = firmwareName_.rfind("S3SDR", 0) == 0 && transport.find("USB") != std::string::npos;

    std::string range;
    if (command(serialFd_, "RANGE?", range, "RANGE ")) {
        double low = 0.0, high = 0.0;
        if (std::sscanf(range.c_str(), "RANGE %lf %lf", &low, &high) == 2) {
            minFreq_ = low * 1e6;
            maxFreq_ = high * 1e6;
        }
    }
    std::string gainInfo;
    if (command(serialFd_, "GAIN?", gainInfo, "GAIN ")) {
        int maximum = 0;
        if (std::sscanf(gainInfo.c_str(), "GAIN %*s %*d %*d %d", &maximum) == 1 && maximum > 0)
            maxGainIndex_ = maximum;
    }

    if (!nativeConfigureStream(err)) { stop(); return false; }
    cb_ = std::move(cb);
    dcOffRe_ = dcOffIm_ = 0.0f;
    running_.store(true);
    serialThread_ = std::thread([this] { nativePump(); });
    return true;
}

bool EspSdrSource::nativeConfigureStream(std::string& err)
{
    std::string line;
    const double corrected = centerFreq_ * (1.0 + ppm_ * 1e-6);
    const int mhz = std::max(100, std::min(6000, (int)std::llround(corrected / 1e6)));
    if (!command(serialFd_, "FREQ " + std::to_string(mhz), line, "OK")) {
        err = "ESP-SDR frequency command failed";
        return false;
    }
    if (firmwareName_.rfind("S3SDR", 0) == 0) {
        const int khz = (int)std::llround((corrected - mhz * 1e6) / 1000.0);
        if (!command(serialFd_, "FOFS " + std::to_string(khz), line, "OK")) {
            err = "ESP-SDR frequency-offset command failed";
            return false;
        }
        centerFreq_ = mhz * 1e6 + khz * 1000.0;
    } else centerFreq_ = mhz * 1e6;
    const std::string gain = gainIndex_ < 0 ? "GAIN HARDWARE" : "GAIN MANUAL " + std::to_string((int)std::llround(gainIndex_));
    if (!command(serialFd_, gain, line, "OK")) {
        err = "ESP-SDR gain command failed";
        return false;
    }

    if (continuous_) {
        const int decim = nearestDecimation(sampleRate_);
        // Experimental high-rate modes trade quantization depth for USB
        // bandwidth: 500 kS/s uses IQ4 and 2 MS/s packs four IQ1 samples/byte.
        const int bits = decim == 8 ? 1 : decim == 32 ? 4 : 8;
        const std::string shift = decim == 8 ? " 0" : "";
        if (!command(serialFd_, "IQS 0 " + std::to_string(decim) + " " + std::to_string(bits) + " 6" + shift, line, "IQS ")) {
            err = "ESP-SDR IQ stream command failed";
            return false;
        }
        sampleRate_ = 16000000.0 / decim;
    } else sampleRate_ = 16000000.0;
    return true;
}

bool EspSdrSource::nativeRestartStream()
{
    if (serialFd_ < 0 || !running_.exchange(false)) return false;
    if (continuous_) writeAll(serialFd_, "\n", 1);
    if (serialThread_.joinable()) serialThread_.join();
    ::tcflush(serialFd_, TCIFLUSH);

    std::string line;
    const std::string nonce = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    if (!command(serialFd_, "SYNC " + nonce, line, ("SYNC " + nonce).c_str())) return false;
    std::string err;
    if (!nativeConfigureStream(err)) return false;
    running_.store(true);
    serialThread_ = std::thread([this] { nativePump(); });
    return true;
}

void EspSdrSource::stop()
{
    const bool wasRunning = running_.exchange(false);
    if (serialFd_ >= 0 && wasRunning) writeAll(serialFd_, "\n", 1);
    if (serialThread_.joinable()) serialThread_.join();
    if (serialFd_ >= 0) { ::close(serialFd_); serialFd_ = -1; }
    cb_ = nullptr;
}

void EspSdrSource::nativePump()
{
    while (running_.load()) {
        if (continuous_) {
            unsigned char header[24];
            if (!readExact(serialFd_, header, sizeof(header), &running_)) break;
            if (std::memcmp(header, "IQS1", 4) != 0) continue;
            const int samples = header[16] | (header[17] << 8);
            const int bits = header[18];
            const int bytes = bits == 1 ? (samples + 3) / 4 : bits == 2 ? (samples + 1) / 2 : bits == 4 ? samples : samples * 2 * (bits / 8);
            if (bytes <= 0 || bytes > (1 << 20)) continue;
            std::vector<unsigned char> payload((size_t)bytes + 4);
            if (!readExact(serialFd_, payload.data(), payload.size(), &running_)) break;
            if (bits == 1 || bits == 2 || bits == 4 || bits == 8 || bits == 16)
                processSamples(payload.data(), bytes, bits, bits == 16 ? 32768 : bits == 8 ? 128 : bits == 4 ? 8 : bits == 2 ? 2 : 1);
        } else {
            const char request[] = "CAP16 16380 6\n";
            if (!writeAll(serialFd_, request, sizeof(request) - 1)) break;
            std::string line;
            if (!readLine(serialFd_, line, &running_)) break;
            int samples = 0; unsigned crc = 0;
            if (std::sscanf(line.c_str(), "DATA %d %x", &samples, &crc) != 2 || samples <= 0 || samples > 16380) continue;
            std::vector<unsigned char> payload((size_t)samples * 2);
            if (!readExact(serialFd_, payload.data(), payload.size(), &running_)) break;
            processSamples(payload.data(), (int)payload.size(), 8, 128);
        }
    }
    running_.store(false);
}

#else

EspSdrSource::~EspSdrSource() { stop(); }
std::vector<SdrDeviceInfo> EspSdrSource::listDevices() { return {}; }
void EspSdrSource::setCenterFreq(double hz) { centerFreq_ = hz; }
void EspSdrSource::setSampleRate(double hz) { sampleRate_ = hz; }
void EspSdrSource::setGain(double gainIndex) { gainIndex_ = gainIndex; }
void EspSdrSource::setPpm(double ppm) { ppm_ = ppm; }
bool EspSdrSource::start(int, SdrSampleCb, std::string& err)
{
    err = "Native ESP-SDR serial transport is not implemented on Windows yet";
    return false;
}
void EspSdrSource::stop() { running_.store(false); cb_ = nullptr; }

#endif

void EspSdrSource::processSamples(const unsigned char* data, int len, int bits, int fullScale)
{
    if (!running_.load() || !cb_ || (bits != 1 && bits != 2 && bits != 4 && bits != 8 && bits != 16)) return;
    const int bytesPerItem = bits / 8;
    const int count = bits == 1 ? len * 4 : bits == 2 ? len * 2 : bits == 4 ? len : len / (bytesPerItem * 2);
    if (count <= 0) return;
    scratch_.resize((size_t)count * 2);
    const float scale = 1.0f / (float)std::max(1, fullScale);
    float offRe = dcOffRe_, offIm = dcOffIm_;
    const bool dc = dcBlock_.load();
    for (int i = 0; i < count; ++i)
    {
        int re, im;
        if (bits == 1)
        {
            const unsigned pair = (data[i / 4] >> (2 * (i & 3))) & 3;
            re = (pair & 1) ? 1 : -1;
            im = (pair & 2) ? 1 : -1;
        }
        else if (bits == 2)
        {
            const unsigned char packed = data[i / 2];
            const unsigned char nibble = (i & 1) ? packed >> 4 : packed & 0x0f;
            re = nibble & 3; if (re & 2) re -= 4;
            im = (nibble >> 2) & 3; if (im & 2) im -= 4;
        }
        else if (bits == 4)
        {
            const unsigned char packed = data[i];
            re = (int)(int8_t)(packed << 4) >> 4;
            im = (int)(int8_t)(packed & 0xf0) >> 4;
        }
        else if (bits == 8)
        {
            re = (int)(int8_t)data[i * 2];
            im = (int)(int8_t)data[i * 2 + 1];
        }
        else
        {
            const size_t p = (size_t)i * 4;
            re = (int)(int16_t)((uint16_t)data[p] | ((uint16_t)data[p + 1] << 8));
            im = (int)(int16_t)((uint16_t)data[p + 2] | ((uint16_t)data[p + 3] << 8));
        }
        // ESP-SDR's raw RF ordering is conjugated: RF above the LO appears at
        // negative baseband frequency. Restore the conventional I+jQ layout.
        im = -im;
        float fre = (float)re * scale, fim = (float)im * scale;
        if (dc)
        {
            float ore = fre - offRe, oim = fim - offIm;
            offRe += ore * dcRate_;
            offIm += oim * dcRate_;
            fre = ore; fim = oim;
        }
        scratch_[(size_t)i * 2] = fre;
        scratch_[(size_t)i * 2 + 1] = fim;
    }
    dcOffRe_ = offRe; dcOffIm_ = offIm;
    cb_(scratch_.data(), count);
}
