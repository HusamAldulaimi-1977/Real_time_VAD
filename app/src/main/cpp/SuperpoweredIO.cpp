#include "SuperpoweredIO.h"

#include <android/log.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <string>
#include <thread>
#include <mutex>
#include <time.h>
#include <sys/stat.h>
#include <vector>

#include "AudioConfig.h"
#include "AudioPipeline.h"

extern "C" {
#include "FIRFilter.h"
}

#include "SuperpoweredAndroidAudioIO.h"
#include "SuperpoweredCPU.h"
#include "Superpowered.h"
#include "SuperpoweredAdvancedAudioPlayer.h"
#include "SuperpoweredSimple.h"
#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_AndroidConfiguration.h>

#define TAG "RT_VAD_SUPERPOWERED"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

static SuperpoweredAndroidAudioIO *gAudioIO = nullptr;
static SuperpoweredAndroidAudioIO *gFileAudioIO = nullptr;
static AudioPipeline* gPipe = nullptr;
struct ResampleState {
    double pos = 0.0;
    float prev = 0.0f;
    bool primed = false;
    FIR* fir = nullptr;
    int firN = 0;
};

static ResampleState gMicState;

static float* gInputFloat = nullptr;
static float* gLeft = nullptr;
static float* gRight = nullptr;
static std::vector<float> gFiltered;
static std::vector<float> gOut16k;
static int gBufferFrames = 0;

static Superpowered::AdvancedAudioPlayer* gFilePlayer = nullptr;
static ResampleState gFileState;
static std::atomic<bool> gFileMonitor{true};
static std::atomic<bool> gFileActive{false};
static std::mutex gFileMutex;
static std::atomic<int> gFileEmptyCount{0};
static std::atomic<bool> gSuperpoweredInit{false};

static void ensureSuperpoweredInit() {
    bool expected = false;
    if (gSuperpoweredInit.compare_exchange_strong(expected, true, std::memory_order_relaxed)) {
        Superpowered::Initialize("ExampleLicenseKey-WillExpire-OnNextUpdate");
    }
}
static float* gFileInterleaved = nullptr;
static float* gFileLeft = nullptr;
static float* gFileRight = nullptr;
static int gFileBufferFrames = 0;
static std::vector<float> gFileFiltered;
static std::vector<float> gFileOut16k;
static std::atomic<long long> gTotalNs{0};
static std::atomic<long long> gTotalRuns{0};
static std::atomic<int> gMicLogCounter{0};
static std::atomic<float> gMicRms{0.0f};

static long long nowNs() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

// Debug-only decimator by fixed factor.
static inline int decimate48kTo16k(ResampleState& st, const float* inMono, int inFrames,
                                   float* filtered, float* out16k) {

    //Your code starts here
    const int D = AudioConfig::DECIMATION_FACTOR; // 3 for 48k -> 16k
    if (inFrames <= 0) return 0;// ensure that the number of input samples in this audio block>0, protect from invalid input.


    if (!st.fir || st.firN != inFrames) {//if input size changed since last time
        if (st.fir) destroyFIR(&st.fir);
        st.fir = initFIR(inFrames);
        st.firN = inFrames;//store the current input size
    }

    // 1) Anti-alias filtering at 48 kHz
    processFIRFilter(st.fir, const_cast<float*>(inMono), filtered);

    // 2) Downsample by keeping every D-th sample
    int outCount = 0;//index for the output array
    for (int i = 0; i < inFrames; i += D) {
        out16k[outCount++] = filtered[i];//i = 0, 3, 6, 9, 12
    }

    // keep resampler state coherent
    st.prev = inMono[inFrames - 1];//last sample of the current block to process smoothly
    st.primed = true;//processed at least one block,
    st.pos = 0.0;

    return outCount;

    //Your code ends here
}

static int resampleLinear(ResampleState& st, const float* inMono, int inFrames,
                          int inRate, int outRate, float* out, int outCapacity) {
    if (inFrames <= 0 || inRate <= 0 || outCapacity <= 0) return 0;
    if (inRate == outRate) {
        int n = (inFrames < outCapacity) ? inFrames : outCapacity;
        std::memcpy(out, inMono, (size_t)n * sizeof(float));
        st.prev = inMono[inFrames - 1];
        st.primed = true;
        st.pos = 0.0;
        return n;
    }

    const double step = (double)inRate / (double)outRate;
    const int total = inFrames + (st.primed ? 1 : 0);
    if (total < 2) {
        st.prev = inMono[inFrames - 1];
        st.primed = true;
        return 0;
    }

    auto sampleAt = [&](int idx) -> float {
        if (!st.primed) return inMono[idx];
        if (idx == 0) return st.prev;
        return inMono[idx - 1];
    };

    int outCount = 0;
    double pos = st.pos;
    while ((pos + 1.0) < total && outCount < outCapacity) {
        int i0 = (int)pos;
        float frac = (float)(pos - i0);
        float s0 = sampleAt(i0);
        float s1 = sampleAt(i0 + 1);
        out[outCount++] = s0 + frac * (s1 - s0);
        pos += step;
    }

    st.pos = pos - (double)(total - 1);
    if (st.pos < 0.0) st.pos = 0.0;
    st.prev = inMono[inFrames - 1];
    st.primed = true;
    return outCount;
}

static void ensureBuffers(int numberOfSamples, int samplerate) {
    if (numberOfSamples <= 0) return;
    if (gBufferFrames != numberOfSamples) {
        if (gInputFloat) { free(gInputFloat); gInputFloat = nullptr; }
        if (gLeft) { free(gLeft); gLeft = nullptr; }
        if (gRight) { free(gRight); gRight = nullptr; }
        gBufferFrames = numberOfSamples;
        gInputFloat = (float*)malloc((size_t)gBufferFrames * 2 * sizeof(float) + 128);
        gLeft = (float*)malloc((size_t)gBufferFrames * sizeof(float) + 64);
        gRight = (float*)malloc((size_t)gBufferFrames * sizeof(float) + 64);
        gFiltered.assign((size_t)gBufferFrames, 0.0f);
    }

    int outCapacity = (int)std::ceil((double)numberOfSamples * AudioConfig::SR /
                                     (double)((samplerate > 0) ? samplerate : AudioConfig::SR)) + 4;
    if (outCapacity < 8) outCapacity = 8;
    if ((int)gOut16k.size() < outCapacity) gOut16k.assign((size_t)outCapacity, 0.0f);
}

static void ensureFileBuffers(int numberOfSamples, int samplerate) {
    if (numberOfSamples <= 0) return;
    if (gFileBufferFrames != numberOfSamples) {
        if (gFileInterleaved) { free(gFileInterleaved); gFileInterleaved = nullptr; }
        if (gFileLeft) { free(gFileLeft); gFileLeft = nullptr; }
        if (gFileRight) { free(gFileRight); gFileRight = nullptr; }
        gFileBufferFrames = numberOfSamples;
        gFileInterleaved = (float*)malloc((size_t)gFileBufferFrames * 2 * sizeof(float) + 128);
        gFileLeft = (float*)malloc((size_t)gFileBufferFrames * sizeof(float) + 64);
        gFileRight = (float*)malloc((size_t)gFileBufferFrames * sizeof(float) + 64);
        gFileFiltered.assign((size_t)gFileBufferFrames, 0.0f);
    }
    int outCapacity = (int)std::ceil((double)numberOfSamples * AudioConfig::SR /
                                     (double)((samplerate > 0) ? samplerate : AudioConfig::SR)) + 4;
    if (outCapacity < 8) outCapacity = 8;
    if ((int)gFileOut16k.size() < outCapacity) gFileOut16k.assign((size_t)outCapacity, 0.0f);
}

// This is called periodically by the media server.
static bool audioCallback(void *clientdata,
                          short int *audio,
                          int numberOfSamples,
                          int samplerate) {
    (void)clientdata;
    if (!audio || numberOfSamples <= 0) return true;
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true)) {
        LOGI("audioCallback first: frames=%d sr=%d", numberOfSamples, samplerate);
    }

    timespec t0{};
    timespec t1{};
    clock_gettime(CLOCK_MONOTONIC, &t0);

    ensureBuffers(numberOfSamples, samplerate);
    if (!gInputFloat || !gLeft || !gRight) return true;

    Superpowered::ShortIntToFloat(audio, gInputFloat, (unsigned int)numberOfSamples);
    Superpowered::DeInterleave(gInputFloat, gLeft, gRight, (unsigned int)numberOfSamples);

    // Simple RMS meter for input diagnostics.
    float sum = 0.0f;
    for (int i = 0; i < numberOfSamples; i++) {
        float v = gLeft[i];
        sum += v * v;
    }
    float rms = (numberOfSamples > 0) ? std::sqrt(sum / (float)numberOfSamples) : 0.0f;
    gMicRms.store(rms, std::memory_order_relaxed);
    int c = gMicLogCounter.fetch_add(1, std::memory_order_relaxed);
    if (c % 100 == 0) {
        LOGI("mic rms=%f", rms);
    }

    int outCount = 0;
    const bool canUseFixedDecimator =
            (samplerate == AudioConfig::SR * AudioConfig::DECIMATION_FACTOR);
    if (canUseFixedDecimator) {
        outCount = decimate48kTo16k(gMicState, gLeft, numberOfSamples,
                                    gFiltered.data(), gOut16k.data());
    } else {
        outCount = resampleLinear(gMicState, gLeft, numberOfSamples, samplerate,
                                  AudioConfig::SR, gOut16k.data(), (int)gOut16k.size());
    }

    if (gPipe) {
        long long tFeedNs = nowNs();
        gPipe->push16kMonoTimed(gOut16k.data(), outCount, tFeedNs);
    }
    // Mute output to avoid mic feedback/buzzing in VAD mode.
    std::memset(audio, 0, (size_t)numberOfSamples * 2 * sizeof(short int));

    clock_gettime(CLOCK_MONOTONIC, &t1);
    long long dtNs = (t1.tv_sec - t0.tv_sec) * 1000000000LL + (t1.tv_nsec - t0.tv_nsec);
    if (dtNs > 0) {
        gTotalNs.fetch_add(dtNs, std::memory_order_relaxed);
        gTotalRuns.fetch_add(1, std::memory_order_relaxed);
    }
    return true;
}

static bool fileOutputCallback(void* clientdata,
                               short int* audio,
                               int numberOfSamples,
                               int samplerate) {
    (void)clientdata;
    timespec t0{};
    timespec t1{};
    clock_gettime(CLOCK_MONOTONIC, &t0);
    auto recordTimeAndReturn = [&](bool result) -> bool {
        clock_gettime(CLOCK_MONOTONIC, &t1);
        long long dtNs = (t1.tv_sec - t0.tv_sec) * 1000000000LL + (t1.tv_nsec - t0.tv_nsec);
        if (dtNs > 0) {
            gTotalNs.fetch_add(dtNs, std::memory_order_relaxed);
            gTotalRuns.fetch_add(1, std::memory_order_relaxed);
        }
        return result;
    };
    if (!audio || numberOfSamples <= 0) return true;
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true)) {
        LOGI("fileOutputCallback first: frames=%d sr=%d", numberOfSamples, samplerate);
    }
    std::unique_lock<std::mutex> lk(gFileMutex, std::try_to_lock);
    if (!lk.owns_lock()) {
        std::memset(audio, 0, (size_t)numberOfSamples * 2 * sizeof(short int));
        return recordTimeAndReturn(true);
    }
    if (!gFileActive.load(std::memory_order_relaxed)) {
        std::memset(audio, 0, (size_t)numberOfSamples * 2 * sizeof(short int));
        return recordTimeAndReturn(true);
    }
    if (!gFilePlayer) {
        std::memset(audio, 0, (size_t)numberOfSamples * 2 * sizeof(short int));
        return recordTimeAndReturn(true);
    }
    ensureFileBuffers(numberOfSamples, samplerate);
    gFilePlayer->outputSamplerate = (unsigned int)samplerate;
    bool hasAudio = gFilePlayer->processStereo(gFileInterleaved, false, (unsigned int)numberOfSamples);
    if (!hasAudio) {
        int empty = gFileEmptyCount.fetch_add(1, std::memory_order_relaxed) + 1;
        if (empty > 50) {
            gFileActive.store(false, std::memory_order_relaxed);
        }
        std::memset(audio, 0, (size_t)numberOfSamples * 2 * sizeof(short int));
        return recordTimeAndReturn(true);
    }
    gFileEmptyCount.store(0, std::memory_order_relaxed);
    Superpowered::DeInterleave(gFileInterleaved, gFileLeft, gFileRight, (unsigned int)numberOfSamples);
    for (int i = 0; i < numberOfSamples; i++) {
        gFileLeft[i] = 0.5f * (gFileLeft[i] + gFileRight[i]); // mono mix
    }
    int outCount = 0;
    const bool canUseFixedDecimator =
            (samplerate == AudioConfig::SR * AudioConfig::DECIMATION_FACTOR);
    static std::atomic<bool> loggedResamplePath{false};
    if (!loggedResamplePath.exchange(true)) {
        LOGI("fileOutputCallback resample path: %s (sr=%d, target=%d)",
             canUseFixedDecimator ? "decimate x3" : "linear",
             samplerate, AudioConfig::SR);
    }
    if (canUseFixedDecimator) {
        outCount = decimate48kTo16k(gFileState, gFileLeft, numberOfSamples,
                                    gFileFiltered.data(), gFileOut16k.data());
    } else {
        outCount = resampleLinear(gFileState, gFileLeft, numberOfSamples, samplerate,
                                  AudioConfig::SR, gFileOut16k.data(), (int)gFileOut16k.size());
    }
    if (gPipe && outCount > 0) {
        long long tFeedNs = nowNs();
        gPipe->push16kMonoTimed(gFileOut16k.data(), outCount, tFeedNs);
    }
    if (!gFileMonitor.load(std::memory_order_relaxed)) {
        std::memset(audio, 0, (size_t)numberOfSamples * 2 * sizeof(short int));
        return recordTimeAndReturn(true);
    }
    Superpowered::FloatToShortInt(gFileInterleaved, audio, (unsigned int)numberOfSamples);
    return recordTimeAndReturn(true);
}

bool SuperpoweredIO_Start(AudioPipeline* pipe, int sampleRate, int bufferSize) {
    if (gAudioIO) return false;
    ensureSuperpoweredInit();
    gPipe = pipe;
    gMicState.pos = 0.0;
    gMicState.prev = 0.0f;
    gMicState.primed = false;
    gTotalNs.store(0, std::memory_order_relaxed);
    gTotalRuns.store(0, std::memory_order_relaxed);

    Superpowered::CPU::setSustainedPerformanceMode(true);

    const int inputStreamType = SL_ANDROID_RECORDING_PRESET_VOICE_RECOGNITION;
    const int outputStreamType = SL_ANDROID_STREAM_MEDIA;
    const int ioSampleRate = (sampleRate > 0) ? sampleRate : AudioConfig::IO_SR;
    const int ioBufferSize = (bufferSize > 0) ? bufferSize : AudioConfig::IO_BUFFER_SIZE;

    LOGI("SuperpoweredIO_Start sr=%d buf=%d", ioSampleRate, ioBufferSize);
    gAudioIO = new SuperpoweredAndroidAudioIO(
            ioSampleRate,
            ioBufferSize,
            true,
            true,
            audioCallback,
            nullptr,
            inputStreamType,
            outputStreamType
    );

    if (!gAudioIO) {
        LOGE("Failed to start SuperpoweredAndroidAudioIO");
        return false;
    }
    LOGI("Superpowered started");
    return true;
}

void SuperpoweredIO_Stop() {
    {
        std::lock_guard<std::mutex> lk(gFileMutex);
        gFileActive.store(false, std::memory_order_relaxed);
        gFileEmptyCount.store(0, std::memory_order_relaxed);
    }
    if (gAudioIO) {
        delete gAudioIO;
        gAudioIO = nullptr;
    }
    if (gMicState.fir) destroyFIR(&gMicState.fir);
    gMicState.firN = 0;
    gBufferFrames = 0;
    if (gInputFloat) { free(gInputFloat); gInputFloat = nullptr; }
    if (gLeft) { free(gLeft); gLeft = nullptr; }
    if (gRight) { free(gRight); gRight = nullptr; }
    gFiltered.clear();
    gOut16k.clear();
    gMicState.pos = 0.0;
    gMicState.prev = 0.0f;
    gMicState.primed = false;
    gTotalNs.store(0, std::memory_order_relaxed);
    gTotalRuns.store(0, std::memory_order_relaxed);
    gFileActive.store(false, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lk(gFileMutex);
        if (gFileAudioIO) {
            delete gFileAudioIO;
            gFileAudioIO = nullptr;
        }
        if (gFilePlayer) {
            delete gFilePlayer;
            gFilePlayer = nullptr;
        }
        if (gFileState.fir) destroyFIR(&gFileState.fir);
        gFileState.firN = 0;
        gFileState.pos = 0.0;
        gFileState.prev = 0.0f;
        gFileState.primed = false;
        gFileMonitor.store(true, std::memory_order_relaxed);
        gFileActive.store(false, std::memory_order_relaxed);
        if (gFileInterleaved) { free(gFileInterleaved); gFileInterleaved = nullptr; }
        if (gFileLeft) { free(gFileLeft); gFileLeft = nullptr; }
        if (gFileRight) { free(gFileRight); gFileRight = nullptr; }
        gFileFiltered.clear();
        gFileOut16k.clear();
        gFileBufferFrames = 0;
    }
    gPipe = nullptr;
    LOGI("Superpowered stopped");
}

float SuperpoweredIO_GetAvgCallbackMs() {
    long long runs = gTotalRuns.load(std::memory_order_relaxed);
    if (runs <= 0) return 0.0f;
    long long totalNs = gTotalNs.load(std::memory_order_relaxed);
    return (float)((double)totalNs / (double)runs / 1000000.0);
}

float SuperpoweredIO_GetMicRms() {
    return gMicRms.load(std::memory_order_relaxed);
}

bool SuperpoweredIO_StartFileMode(AudioPipeline* pipe, const char* path, int bufferSize, int outSampleRate) {
    if (!path || !pipe) return false;
    ensureSuperpoweredInit();
    gPipe = pipe;
    std::lock_guard<std::mutex> lk(gFileMutex);
    gFileActive.store(false, std::memory_order_relaxed);
    if (gFileState.fir) destroyFIR(&gFileState.fir);
    gFileState.fir = nullptr;
    gFileState.firN = 0;
    gFileState.pos = 0.0;
    gFileState.prev = 0.0f;
    gFileState.primed = false;
    const int ioBufferSize = (bufferSize > 0) ? bufferSize : 0;
    const int ioSampleRate = (outSampleRate > 0) ? outSampleRate : AudioConfig::IO_SR;
    if (gFileAudioIO) {
        delete gFileAudioIO;
        gFileAudioIO = nullptr;
    }
    if (gFilePlayer) {
        delete gFilePlayer;
        gFilePlayer = nullptr;
    }
    gFilePlayer = new Superpowered::AdvancedAudioPlayer((unsigned int)ioSampleRate, 0);
    if (!gFilePlayer) {
        LOGE("Failed to create AdvancedAudioPlayer");
        return false;
    }
    gFilePlayer->open(path);
    gFilePlayer->play();
    gFileActive.store(true, std::memory_order_relaxed);
    gFileEmptyCount.store(0, std::memory_order_relaxed);
    LOGI("SuperpoweredIO_StartFileMode sr=%d buf=%d", ioSampleRate, ioBufferSize);
    gFileAudioIO = new SuperpoweredAndroidAudioIO(
            ioSampleRate,
            ioBufferSize,
            false,
            true,
            fileOutputCallback,
            nullptr,
            -1,
            SL_ANDROID_STREAM_MEDIA
    );
    return gFileAudioIO != nullptr;
}

void SuperpoweredIO_SetFileMonitor(bool enabled) {
    std::lock_guard<std::mutex> lk(gFileMutex);
    gFileMonitor.store(enabled, std::memory_order_relaxed);
}

bool SuperpoweredIO_IsFilePlaying() {
    std::lock_guard<std::mutex> lk(gFileMutex);
    if (!gFileActive.load(std::memory_order_relaxed)) return false;
    if (!gFilePlayer) return false;
    if (gFilePlayer->isPlaying()) return true;
    return gFileEmptyCount.load(std::memory_order_relaxed) <= 50;
}

