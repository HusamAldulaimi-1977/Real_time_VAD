//
// Created by nafis on 2/15/2026.
//

#include "AudioPipeline.h"
#include "FeatureExtractor.h"
#include "AudioConfig.h"
#include <cmath>
#include <android/log.h>
#include <time.h>

#define LOG_TAG "AudioPipeline"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)


AudioPipeline::AudioPipeline(int ringCapacitySamples16k)
        : ring((size_t)ringCapacitySamples16k),
          featRing(AudioConfig::SLIDE_W),    // allocate sliding window length
          featWrite(0),
          featCount(0),
          stridePhase(0) {}

AudioPipeline::~AudioPipeline() { stop(); }

void AudioPipeline::start() {
    if (run.load()) return;
    ring.reset();
    featWrite = 0;
    featCount = 0;
    stridePhase = 0;
    hasFeat.store(false, std::memory_order_relaxed);
    drops.store(0);
    pushed.store(0);
    lastRms.store(0.0f);
    featTotalNs.store(0);
    featRuns.store(0);

    {
        std::lock_guard<std::mutex> lk(timingMu_);
        hopTsQueue_.clear();
        hopTsRing_.clear();
        timingSampleAcc_ = 0;
    }

    run.store(true);
    worker = std::thread(&AudioPipeline::workerLoop, this);
}

void AudioPipeline::stop() {
    run.store(false);
    cv.notify_all();
    if (worker.joinable()) worker.join();
}

bool AudioPipeline::getLatestFeatures(FeatureFrame& out) const {
    if (!hasFeat.load(std::memory_order_relaxed)) return false;
    std::lock_guard<std::mutex> lk(featMu);
    out = latestFeat;
    return true;
}

void AudioPipeline::push16kMono(const float* x, int n) {
    size_t pushedNow = 0, droppedNow = 0;
    {
        std::lock_guard<std::mutex> lk(mu);
        pushedNow = ring.push(x, (size_t)n);
        if (pushedNow < (size_t)n) droppedNow = (size_t)n - pushedNow;
    }
    if (pushedNow) {
        pushed.fetch_add((long long)pushedNow);
        cv.notify_one();
    }
    if (droppedNow) drops.fetch_add((int)droppedNow);
}

void AudioPipeline::push16kMonoTimed(const float* x, int n, long long t0Ns) {
    push16kMono(x, n);
    if (n <= 0) return;
    const double nsPerSample = 1000000000.0 / (double)AudioConfig::SR;
    int offset = 0;
    std::lock_guard<std::mutex> lk(timingMu_);
    while (offset < n) {
        if (timingSampleAcc_ == 0) {
            long long hopStartNs = t0Ns + (long long)(offset * nsPerSample);
            hopTsQueue_.push_back(hopStartNs);
            if (hopTsQueue_.size() > 4096) hopTsQueue_.pop_front();
        }
        int remaining = n - offset;
        int need = AudioConfig::HOP - timingSampleAcc_;
        int take = (remaining < need) ? remaining : need;
        timingSampleAcc_ += take;
        offset += take;
        if (timingSampleAcc_ >= AudioConfig::HOP) timingSampleAcc_ = 0;
    }
}

void AudioPipeline::pushFeatureFrame(const FeatureFrame& ff) {
    featRing[featWrite] = ff;
    featWrite = (featWrite + 1) % AudioConfig::SLIDE_W;
    if (featCount < AudioConfig::SLIDE_W) featCount++;
}

bool AudioPipeline::buildWindow(float* outCW, int usedChannels) const {
    if (featCount < AudioConfig::SLIDE_W) return false;

    // oldest index is featWrite (next write position)
    int idx = featWrite;
    for (int t = 0; t < AudioConfig::SLIDE_W; t++) {
        const auto& ff = featRing[idx];
        // ff.feat is assumed length = C_USED
        for (int c = 0; c < usedChannels; c++) {
            outCW[c * AudioConfig::SLIDE_W + t] = ff.feat[c]; // [c][t]
        }
        idx = (idx + 1) % AudioConfig::SLIDE_W;
    }
    return true;
}
static inline float clamp01(float x) {
    if (x < 0.f) return 0.f;
    if (x > 1.f) return 1.f;
    return x;
}

static void normalizeWindow(
        float* CW, int C_USED, // CW is [C][SLIDE_W] stored as CW[c*SLIDE_W+t]
        float mn, float mx) {

    const float eps = 1e-12f;
    float den = (mx - mn) + eps;
    for (int c = 0; c < C_USED; c++) {
        for (int t = 0; t < AudioConfig::SLIDE_W; t++) {
            float v = (CW[c * AudioConfig::SLIDE_W + t] - mn) / den;
            CW[c * AudioConfig::SLIDE_W + t] = clamp01(v);
        }
    }
}

void AudioPipeline::setTrainStats(float mn_, float mx_,
                                  float mseThr_) {
    mn.store(mn_); mx.store(mx_);
    mseThr.store(mseThr_);
    hasStats.store(true);
}

bool AudioPipeline::getLatestDecision(int& outDecision, float& outMse) const {
    int d = latestDecision.load(std::memory_order_relaxed);
    if (d < 0) return false;
    outDecision = d;
    outMse = latestMse.load(std::memory_order_relaxed);
    return true;
}

bool AudioPipeline::popFeatureWindow(std::vector<float>& out) {
    return popFeatureWindow(out, nullptr);
}

bool AudioPipeline::popFeatureWindow(std::vector<float>& out, long long* outStartNs) {
    std::lock_guard<std::mutex> lk(winMutex_);
    if (winQueue_.empty()) return false;
    WindowItem item = std::move(winQueue_.front());
    winQueue_.pop_front();
    out = std::move(item.data);
    if (outStartNs) *outStartNs = item.startNs;
    return true;
}


void AudioPipeline::submitMSE(float mse) {
    latestMse.store(mse, std::memory_order_relaxed);
    float thr = mseThr.load(std::memory_order_relaxed);
    int decision = (mse < thr) ? 1 : 0;
    latestDecision.store(decision, std::memory_order_relaxed);
}

void AudioPipeline::setThreshold(float thr) { threshold_.store(thr); }
float AudioPipeline::getLatestMSE() const { return latestMSE_.load(); }

float AudioPipeline::getAvgFeatureMs() const {
    long long runs = featRuns.load(std::memory_order_relaxed);
    if (runs <= 0) return 0.0f;
    long long total = featTotalNs.load(std::memory_order_relaxed);
    return (float)((double)total / (double)runs / 1000000.0);
}

void AudioPipeline::workerLoop() {
    const int chunk = AudioConfig::HOP;
    std::vector<float> x(chunk);

    while (run.load()) {
        {
            std::unique_lock<std::mutex> lk(mu);
            cv.wait(lk, [&]{ return !run.load() || ring.availableToRead() >= (size_t)chunk; });
            if (!run.load()) break;
            if (!ring.pop(x.data(), (size_t)chunk)) continue;
        }



        timespec t0{};
        timespec t1{};
        clock_gettime(CLOCK_MONOTONIC, &t0);

        const int usedChannels = AudioConfig::N_MFCC;
        FeatureFrame ff;
        if (feat.processHop(x.data(), ff)) {
            long long hopTs = -1;
            {
                std::lock_guard<std::mutex> lk(timingMu_);
                if (!hopTsQueue_.empty()) {
                    hopTs = hopTsQueue_.front();
                    hopTsQueue_.pop_front();
                }
                if (hopTs >= 0) {
                    hopTsRing_.push_back(hopTs);
                    if (hopTsRing_.size() > AudioConfig::SLIDE_W) hopTsRing_.pop_front();
                }
            }
            {
                std::lock_guard<std::mutex> lk(featMu);
                latestFeat = ff;
            }
            hasFeat.store(true, std::memory_order_relaxed);
            // (B) feed the 64-frame window ring
            pushFeatureFrame(ff);

            if (hasStats.load(std::memory_order_relaxed)) {
                if (featCount >= AudioConfig::SLIDE_W) {

                    bool emit = false;

                    if (featCount == AudioConfig::SLIDE_W) {
                        emit = true;   // emit first full window immediately
                        stridePhase = 1 % AudioConfig::SLIDE_STRIDE;
                    } else {
                        if (stridePhase == 0) emit = true;
                        stridePhase = (stridePhase + 1) % AudioConfig::SLIDE_STRIDE;
                    }

                    if (emit) {
                        const int C_USED = usedChannels;
                        std::vector<float> CW((size_t)C_USED * AudioConfig::SLIDE_W);

                        if (buildWindow(CW.data(), C_USED)) {

                            normalizeWindow(
                                    CW.data(), C_USED,
                                    mn.load(std::memory_order_relaxed),
                                    mx.load(std::memory_order_relaxed)
                            );

                                                        std::vector<float> flat((size_t)C_USED * AudioConfig::SLIDE_W);
                            for (int c = 0; c < C_USED; c++) {
                                for (int t = 0; t < AudioConfig::SLIDE_W; t++) {
                                    flat[c * AudioConfig::SLIDE_W + t] = CW[c * AudioConfig::SLIDE_W + t];
                                }
                            }

                            long long windowStartNs = -1;
                            {
                                std::lock_guard<std::mutex> lk(timingMu_);
                                if (hopTsRing_.size() >= AudioConfig::SLIDE_W) windowStartNs = hopTsRing_.front();
                            }

                            {
                                std::lock_guard<std::mutex> lk(winMutex_);
                                if (winQueue_.size() > 6) winQueue_.pop_front();
                                WindowItem item;
                                item.data = std::move(flat);
                                item.startNs = windowStartNs;
                                winQueue_.push_back(std::move(item));
                            }
                        }
                    }
                }
            }

            // optional debug checksum:
            float s = 0.0f;
            for (int i = 0; i < usedChannels; i++) s += ff.feat[i];
            lastFeatSum.store(s);
            LOGI("featSum=%f", s);
            clock_gettime(CLOCK_MONOTONIC, &t1);
            long long dtNs = (t1.tv_sec - t0.tv_sec) * 1000000000LL + (t1.tv_nsec - t0.tv_nsec);
            if (dtNs > 0) {
                featTotalNs.fetch_add(dtNs);
                featRuns.fetch_add(1);
            }
        }
    }
}



