#include "sdr/esp_sdr_source.h"

#include <algorithm>
#include <cstdint>
#include <utility>

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
    if (!running_.load() || !cb_ || (bits != 8 && bits != 16)) return;
    const int bytesPerItem = bits / 8;
    const int count = len / (bytesPerItem * 2);
    if (count <= 0) return;
    scratch_.resize((size_t)count * 2);
    const float scale = 1.0f / (float)std::max(1, fullScale);
    float offRe = dcOffRe_, offIm = dcOffIm_;
    const bool dc = dcBlock_.load();
    for (int i = 0; i < count; ++i)
    {
        int re, im;
        if (bits == 8)
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

#else

EspSdrSource::~EspSdrSource() = default;
std::vector<SdrDeviceInfo> EspSdrSource::listDevices() { return {}; }
void EspSdrSource::setCenterFreq(double hz) { centerFreq_ = hz; }
void EspSdrSource::setSampleRate(double hz) { sampleRate_ = hz; }
void EspSdrSource::setGain(double gainIndex) { gainIndex_ = gainIndex; }
void EspSdrSource::setPpm(double ppm) { ppm_ = ppm; }
bool EspSdrSource::start(int, SdrSampleCb, std::string& err)
{
    err = "ESP-SDR is available in the browser build";
    return false;
}
void EspSdrSource::stop() { running_.store(false); }

#endif
