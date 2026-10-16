// ESP-SDR source for the browser build. Boards run the receive-only protocol
// from Kelsidavis/esp-sdr-s2-range over Web Serial. S3 native USB provides
// IQS1 streaming; UART-connected boards provide gapped CAP16 snapshots.
#pragma once

#include "sdr/sdr_source.h"

#include <atomic>
#include <functional>
#include <string>
#include <thread>
#include <vector>

class EspSdrSource : public SdrSource
{
public:
    EspSdrSource() = default;
    ~EspSdrSource() override;

    std::vector<SdrDeviceInfo> listDevices() override;
    void setCenterFreq(double hz) override;
    void setSampleRate(double hz) override;
    void setGain(double gainIndex) override;
    void setBiasTee(bool) override {}
    void setPpm(double ppm) override;
    void setDeviceIndex(int index) { deviceIndex_ = index; }

    double centerFreq() const override { return centerFreq_; }
    double sampleRate() const override { return sampleRate_; }
    bool start(int deviceIndex, SdrSampleCb cb, std::string& err) override;
    void stop() override;
    bool running() const override { return running_.load(); }

    void setDcBlock(bool on) { dcBlock_.store(on); }
    int maxGainIndex() const { return maxGainIndex_; }
    bool continuous() const { return continuous_; }
    double minFreq() const { return minFreq_; }
    double maxFreq() const { return maxFreq_; }
    const std::string& firmwareName() const { return firmwareName_; }

#ifdef __EMSCRIPTEN__
    using BrowserPrepareCb = std::function<void(bool ok, const std::string& err)>;
    bool beginBrowserPrepare(int deviceIndex, BrowserPrepareCb cb, std::string& err);
    void onBrowserPrepareResult(bool ok, const std::string& msg, double actualRate,
                                double actualCenter, double minFreq, double maxFreq,
                                int maxGainIndex, bool continuous);
    void onSamples(const unsigned char* data, int len, int bits, int fullScale);
#endif

private:
    void processSamples(const unsigned char* data, int len, int bits, int fullScale);
#if !defined(__EMSCRIPTEN__) && !defined(_WIN32)
    void nativePump();
    bool nativeConfigureStream(std::string& err);
    bool nativeRestartStream();
#endif
    std::atomic<bool> running_{false};
    std::atomic<bool> dcBlock_{true};
    SdrSampleCb cb_;
    double centerFreq_ = 1960.0e6;
    double sampleRate_ = 250.0e3;
    double gainIndex_ = -1.0;
    double ppm_ = 0.0;
    double minFreq_ = 0.0;
    double maxFreq_ = 0.0;
    int maxGainIndex_ = 82;
    bool continuous_ = false;
    std::string firmwareName_;
    float dcOffRe_ = 0.0f;
    float dcOffIm_ = 0.0f;
    float dcRate_ = 2.0e-4f;
    std::vector<float> scratch_;
    int deviceIndex_ = 0;

#ifdef __EMSCRIPTEN__
    bool browserPrepared_ = false;
    bool browserPreparing_ = false;
    BrowserPrepareCb browserPrepareCb_;
#elif !defined(_WIN32)
    int serialFd_ = -1;
    std::thread serialThread_;
    std::vector<std::string> nativePaths_;
#endif
};
