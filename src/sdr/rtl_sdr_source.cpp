#include "sdr/rtl_sdr_source.h"

#include <cmath>
#include <cstdlib>
#include <cstring>

#if defined(HAS_RTLSDR_LIB)
#include <rtl-sdr.h>
#endif

#if !defined(HAS_RTLSDR_LIB)

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>

#ifdef signbit
#undef signbit
#endif
#ifdef isfinite
#undef isfinite
#endif
#ifdef isinf
#undef isinf
#endif
#ifdef isnan
#undef isnan
#endif

namespace {
RtlSdrSource* g_browser_source = nullptr;
}

extern "C" {
EMSCRIPTEN_KEEPALIVE void cellscope_web_rtl_prepare_result(int ok, const char* msg)
{
    if (g_browser_source)
        g_browser_source->onBrowserPrepareResult(ok != 0, msg ? std::string(msg) : std::string());
}

EMSCRIPTEN_KEEPALIVE void cellscope_web_rtl_sample_block(const unsigned char* data, int len, double, int)
{
    if (g_browser_source && data && len > 0)
        g_browser_source->onAsync(const_cast<unsigned char*>(data), (uint32_t)len);
}
}

EM_JS(void, web_rtl_configure, (double centerFreq, double sampleRate, double gainDb, int biasTee, double ppm, int directSamplingMethod), {
    Module.cellscopeRtl.configure({
        centerFreq: centerFreq,
        sampleRate: sampleRate,
        gainDb: gainDb,
        biasTee: !!biasTee,
        ppm: ppm,
        directSamplingMethod: directSamplingMethod,
    });
});

EM_JS(void, web_rtl_begin_prepare, (), {
    Module.cellscopeRtl.beginPrepare();
});

EM_JS(void, web_rtl_start_stream, (), {
    Module.cellscopeRtl.startStream();
});

EM_JS(void, web_rtl_stop, (), {
    Module.cellscopeRtl.stop();
});

EM_JS(void, web_rtl_set_center_freq, (double hz), {
    Module.cellscopeRtl.setCenterFreq(hz);
});

EM_JS(void, web_rtl_set_sample_rate, (double hz), {
    Module.cellscopeRtl.setSampleRate(hz);
});

EM_JS(void, web_rtl_set_gain, (double gainDb, int autoGain), {
    Module.cellscopeRtl.setGain(autoGain ? -1 : gainDb);
});

EM_JS(void, web_rtl_set_bias_tee, (int on), {
    Module.cellscopeRtl.setBiasTee(!!on);
});

EM_JS(void, web_rtl_set_ppm, (double ppm), {
    Module.cellscopeRtl.setPpm(ppm);
});

EM_JS(void, web_rtl_set_direct_sampling, (int method), {
    Module.cellscopeRtl.setDirectSampling(method);
});

RtlSdrSource::~RtlSdrSource()
{
    stop();
}

std::vector<SdrDeviceInfo> RtlSdrSource::listDevices()
{
    return {SdrDeviceInfo{0, "WebUSB RTL-SDR", "connect on Start"}};
}

void RtlSdrSource::setCenterFreq(double hz)
{
    centerFreq_ = hz;
    if (browserPrepared_ || running_.load())
        web_rtl_set_center_freq(hz);
}

void RtlSdrSource::setSampleRate(double hz)
{
    sampleRate_ = hz;
    if (browserPrepared_ || running_.load())
        web_rtl_set_sample_rate(hz);
}

void RtlSdrSource::setGain(double db)
{
    gainDb_ = db;
    if (browserPrepared_ || running_.load())
        web_rtl_set_gain(gainDb_, gainDb_ < 0.0 ? 1 : 0);
}

void RtlSdrSource::setBiasTee(bool on)
{
    biasTee_ = on;
    if (browserPrepared_ || running_.load())
        web_rtl_set_bias_tee(on ? 1 : 0);
}

void RtlSdrSource::setPpm(double ppm)
{
    ppm_ = ppm;
    if (browserPrepared_ || running_.load())
        web_rtl_set_ppm(ppm);
}

bool RtlSdrSource::beginBrowserPrepare(int, BrowserPrepareCb cb, std::string& err)
{
    if (browserPreparing_)
    {
        err = "RTL-SDR WebUSB prepare already in progress";
        return false;
    }

    browserPrepareCb_ = std::move(cb);
    browserPrepared_ = false;
    browserPreparing_ = true;
    g_browser_source = this;

    web_rtl_configure(centerFreq_, sampleRate_, gainDb_, biasTee_ ? 1 : 0, ppm_, 0);
    web_rtl_begin_prepare();
    return true;
}

void RtlSdrSource::onBrowserPrepareResult(bool ok, const std::string& err)
{
    browserPreparing_ = false;
    browserPrepared_ = ok;
    if (!ok)
        browserPrepareCb_ = nullptr;
    if (browserPrepareCb_)
        browserPrepareCb_(ok, err);
    if (!ok)
        running_.store(false);
}

bool RtlSdrSource::start(int, SdrSampleCb cb, std::string& err)
{
    if (running_.load())
        return true;
    if (!browserPrepared_)
    {
        err = "RTL-SDR WebUSB device is not prepared";
        return false;
    }

    cb_ = std::move(cb);
    dcOffRe_ = dcOffIm_ = 0.0f;
    dcRate_ = (float)(50.0 / sampleRate_);
    running_.store(true);
    web_rtl_start_stream();
    return true;
}

void RtlSdrSource::stop()
{
    running_.store(false);
    browserPreparing_ = false;
    browserPrepared_ = false;
    browserPrepareCb_ = nullptr;
    web_rtl_stop();
    if (g_browser_source == this)
        g_browser_source = nullptr;
    cb_ = nullptr;
}

void RtlSdrSource::onAsync(unsigned char* buf, uint32_t len)
{
    if (!running_.load() || !cb_)
        return;

    uint32_t num = len / 2;
    if (scratch_.size() < (size_t)num * 2)
        scratch_.resize((size_t)num * 2);

    float* out = scratch_.data();
    bool dc = dcBlock_.load();
    float offRe = dcOffRe_, offIm = dcOffIm_;
    const float rate = dcRate_;
    for (uint32_t i = 0; i < num; ++i)
    {
        float re = ((int)buf[i * 2] - 128) * (1.0f / 128.0f);
        float im = ((int)buf[i * 2 + 1] - 128) * (1.0f / 128.0f);
        if (dc)
        {
            float ore = re - offRe;
            offRe += ore * rate;
            float oim = im - offIm;
            offIm += oim * rate;
            out[i * 2]     = ore;
            out[i * 2 + 1] = oim;
        }
        else
        {
            out[i * 2]     = re;
            out[i * 2 + 1] = im;
        }
    }
    dcOffRe_ = offRe;
    dcOffIm_ = offIm;

    cb_(out, (int)num);
}

#else

RtlSdrSource::~RtlSdrSource() = default;

std::vector<SdrDeviceInfo> RtlSdrSource::listDevices()
{
    return {};
}

void RtlSdrSource::setCenterFreq(double hz) { centerFreq_ = hz; }
void RtlSdrSource::setSampleRate(double hz) { sampleRate_ = hz; }
void RtlSdrSource::setGain(double db) { gainDb_ = db; }
void RtlSdrSource::setBiasTee(bool on) { biasTee_ = on; }
void RtlSdrSource::setPpm(double ppm) { ppm_ = ppm; }

bool RtlSdrSource::start(int, SdrSampleCb, std::string& err)
{
    err = "RTL-SDR support is not available in this build";
    return false;
}

void RtlSdrSource::stop() {}

#endif // __EMSCRIPTEN__

#else

RtlSdrSource::~RtlSdrSource()
{
    stop();
}

std::vector<SdrDeviceInfo> RtlSdrSource::listDevices()
{
    std::vector<SdrDeviceInfo> out;
    uint32_t count = rtlsdr_get_device_count();
    for (uint32_t i = 0; i < count; ++i)
    {
        char manufact[256] = {0}, product[256] = {0}, serial[256] = {0};
        rtlsdr_get_device_usb_strings(i, manufact, product, serial);
        SdrDeviceInfo info;
        info.index = (int)i;
        info.name = std::string(rtlsdr_get_device_name(i));
        info.serial = serial;
        out.push_back(info);
    }
    return out;
}

void RtlSdrSource::applyGain()
{
    if (!dev_)
        return;

    if (gainDb_ < 0.0)
    {
        rtlsdr_set_tuner_gain_mode(dev_, 0); // auto
        rtlsdr_set_agc_mode(dev_, 1);
        return;
    }

    rtlsdr_set_agc_mode(dev_, 0);
    rtlsdr_set_tuner_gain_mode(dev_, 1); // manual

    int num = rtlsdr_get_tuner_gains(dev_, nullptr);
    if (num <= 0)
        return;
    std::vector<int> gains((size_t)num);
    rtlsdr_get_tuner_gains(dev_, gains.data());

    int target = (int)std::lround(gainDb_ * 10.0); // tenths of dB
    int best = gains[0];
    int bestDiff = std::abs(target - gains[0]);
    for (int i = 1; i < num; ++i)
    {
        int diff = std::abs(target - gains[i]);
        if (diff < bestDiff)
        {
            best = gains[i];
            bestDiff = diff;
        }
    }
    rtlsdr_set_tuner_gain(dev_, best);
}

bool RtlSdrSource::start(int deviceIndex, SdrSampleCb cb, std::string& err)
{
    if (running_.load())
        return true;

    uint32_t count = rtlsdr_get_device_count();
    if (count == 0)
    {
        err = "No RTL-SDR devices found";
        return false;
    }
    if (deviceIndex < 0 || (uint32_t)deviceIndex >= count)
    {
        err = "RTL-SDR device index out of range";
        return false;
    }

    if (rtlsdr_open(&dev_, (uint32_t)deviceIndex) < 0 || !dev_)
    {
        err = "Failed to open RTL-SDR device";
        dev_ = nullptr;
        return false;
    }

    // Identify the tuner so we can warn about out-of-range frequencies.
    switch (rtlsdr_get_tuner_type(dev_))
    {
    case RTLSDR_TUNER_E4000:  tunerType_ = "E4000";  tunerMaxFreq_ = 2200e6; break;
    case RTLSDR_TUNER_FC0012: tunerType_ = "FC0012"; tunerMaxFreq_ = 948e6;  break;
    case RTLSDR_TUNER_FC0013: tunerType_ = "FC0013"; tunerMaxFreq_ = 1100e6; break;
    case RTLSDR_TUNER_FC2580: tunerType_ = "FC2580"; tunerMaxFreq_ = 924e6;  break;
    case RTLSDR_TUNER_R820T:  tunerType_ = "R820T";  tunerMaxFreq_ = 1766e6; break;
    case RTLSDR_TUNER_R828D:  tunerType_ = "R828D";  tunerMaxFreq_ = 1766e6; break;
    default:                  tunerType_ = "unknown"; tunerMaxFreq_ = 0.0;   break;
    }

    rtlsdr_set_sample_rate(dev_, (uint32_t)sampleRate_);
    uint32_t actual = rtlsdr_get_sample_rate(dev_);
    if (actual)
        sampleRate_ = (double)actual;

    if (ppm_ != 0.0)
        rtlsdr_set_freq_correction(dev_, (int)std::lround(ppm_));

    rtlsdr_set_center_freq(dev_, (uint32_t)centerFreq_);
    actualCenter_.store((double)rtlsdr_get_center_freq(dev_));
    applyGain();

    rtlsdr_set_bias_tee(dev_, biasTee_ ? 1 : 0);

    rtlsdr_reset_buffer(dev_);

    cb_ = std::move(cb);
    dcOffRe_ = dcOffIm_ = 0.0f;
    dcRate_ = (float)(50.0 / sampleRate_); // ~tens of Hz corner
    running_.store(true);

    readThread_ = std::thread([this]() {
        rtlsdr_read_async(dev_, &RtlSdrSource::asyncTrampoline, this, 15, 32768);
        running_.store(false);
    });

    return true;
}

void RtlSdrSource::stop()
{
    if (dev_)
        rtlsdr_cancel_async(dev_);
    if (readThread_.joinable())
        readThread_.join();
    if (dev_)
    {
        rtlsdr_close(dev_);
        dev_ = nullptr;
    }
    running_.store(false);
    cb_ = nullptr;
}

void RtlSdrSource::asyncTrampoline(unsigned char* buf, uint32_t len, void* ctx)
{
    static_cast<RtlSdrSource*>(ctx)->onAsync(buf, len);
}

void RtlSdrSource::onAsync(unsigned char* buf, uint32_t len)
{
    if (!running_.load() || !cb_)
        return;

    uint32_t num = len / 2;
    if (scratch_.size() < (size_t)num * 2)
        scratch_.resize((size_t)num * 2);

    float* out = scratch_.data();
    bool dc = dcBlock_.load();
    float offRe = dcOffRe_, offIm = dcOffIm_;
    const float rate = dcRate_;
    for (uint32_t i = 0; i < num; ++i)
    {
        float re = ((int)buf[i * 2] - 128) * (1.0f / 128.0f);
        float im = ((int)buf[i * 2 + 1] - 128) * (1.0f / 128.0f);
        if (dc)
        {
            float ore = re - offRe;
            offRe += ore * rate;
            float oim = im - offIm;
            offIm += oim * rate;
            out[i * 2]     = ore;
            out[i * 2 + 1] = oim;
        }
        else
        {
            out[i * 2]     = re;
            out[i * 2 + 1] = im;
        }
    }
    dcOffRe_ = offRe;
    dcOffIm_ = offIm;

    cb_(out, (int)num);
}

void RtlSdrSource::setCenterFreq(double hz)
{
    centerFreq_ = hz;
    if (dev_)
    {
        rtlsdr_set_center_freq(dev_, (uint32_t)hz);
        actualCenter_.store((double)rtlsdr_get_center_freq(dev_));
    }
}

void RtlSdrSource::setSampleRate(double hz)
{
    sampleRate_ = hz;
    if (dev_)
    {
        rtlsdr_set_sample_rate(dev_, (uint32_t)hz);
        uint32_t actual = rtlsdr_get_sample_rate(dev_);
        if (actual)
            sampleRate_ = (double)actual;
    }
}

void RtlSdrSource::setGain(double db)
{
    gainDb_ = db;
    if (dev_)
        applyGain();
}

void RtlSdrSource::setBiasTee(bool on)
{
    biasTee_ = on;
    if (dev_)
        rtlsdr_set_bias_tee(dev_, on ? 1 : 0);
}

void RtlSdrSource::setPpm(double ppm)
{
    ppm_ = ppm;
    if (dev_)
        rtlsdr_set_freq_correction(dev_, (int)std::lround(ppm));
}

#endif
