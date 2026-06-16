//
// Created by nafis on 2/15/2026.
//

#pragma once
#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <vector>
#include <deque>


#include "RingBuffer.h"
#include "FeatureExtractor.h"

class AudioPipeline {
    // --- window queue (for Java/PyTorch inference thread) ---
    struct WindowItem {
        std::vector<float> data;
        long long startNs = -1;
    };
    mutable std::mutex winMutex_;
    std::deque<WindowItem> winQueue_;
public:
    AudioPipeline(int ringCapacitySamples16k);
    ~AudioPipeline();

    void start();
    void stop();

    // Called from audio callback thread
    void push16kMono(const float* x, int n);
    void push16kMonoTimed(const float* x, int n, long long t0Ns);

    // Read-only status for UI/debug
    float getLastRms() const { return lastRms.load(); }
    long long getPushed() const { return pushed.load(); }
    int getDrops() const { return drops.load(); }
    bool getLatestFeatures(FeatureFrame& out) const;
    bool hasFeatures() const { return hasFeat.load(std::memory_order_relaxed); }
    bool getLatestDecision(int& outDecision, float& outMse) const;

    float getMseThreshold() const { return mseThr.load(std::memory_order_relaxed); }

    bool popFeatureWindow(std::vector<float>& out); // called by Java thread via JNI
    bool popFeatureWindow(std::vector<float>& out, long long* outStartNs);
    void setThreshold(float thr);
    float getLatestMSE() const;
    // Java calls this after PyTorch inference:
    void submitMSE(float mse);
    float getAvgFeatureMs() const;

    void setTrainStats(float mn, float mx,
                       float mseThr);



private:
    void workerLoop();

    RingBuffer ring;
    std::mutex mu;
    std::condition_variable cv;

    std::atomic<bool> run{false};
    std::thread worker;

    std::atomic<int> drops{0};
    std::atomic<long long> pushed{0};
    std::atomic<float> lastRms{0.0f};
    std::atomic<long long> featTotalNs{0};
    std::atomic<long long> featRuns{0};

    std::mutex timingMu_;
    std::deque<long long> hopTsQueue_;
    std::deque<long long> hopTsRing_;
    int timingSampleAcc_ = 0;
    FeatureExtractor feat;                 // feature extractor instance
    std::atomic<float> lastFeatSum{0.0f};  // debug: proves features are computed
    mutable std::mutex featMu;
    FeatureFrame latestFeat{};
    std::atomic<bool> hasFeat{false};
    void pushFeatureFrame(const FeatureFrame& ff);
    bool buildWindow(float* outCW, int usedChannels) const; // outCW size = usedChannels*64




    std::atomic<bool>  hasStats{false};
    std::atomic<float> mn{0.f}, mx{1.f};
    std::atomic<float> mseThr{0.f};

    std::atomic<int>   latestDecision{-1};
    std::atomic<float> latestMse{0.f};



    std::atomic<float> threshold_{0.0f};
    std::atomic<float> latestMSE_{0.0f};


    std::vector<FeatureFrame> featRing;
    int featWrite = 0;
    int featCount = 0;
    int stridePhase;
};


