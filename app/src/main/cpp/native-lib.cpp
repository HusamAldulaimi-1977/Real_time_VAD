//
// Created by nafis on 2/7/2026.
#include <jni.h>
#include <android/log.h>
#include <cstring>
#include <vector>
#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <cmath>

#define TAG "RT_VAD_NATIVE"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

#include "RingBuffer.h"
#include "AudioPipeline.h"
#include "FeatureExtractor.h"
#include "AudioConfig.h"
#include "SuperpoweredIO.h"


//static AudioPipeline* gPipe = nullptr;
static AudioPipeline* gPipe = nullptr;
static std::atomic<bool>  gHasStats{false};
static std::atomic<float> gMn{0.0f}, gMx{1.0f};
static std::atomic<float> gMseThr{0.0f};

static std::atomic<int>   gLatestDecision{-1}; // 1 speech, 0 noise, -1 not ready
static std::atomic<float> gLatestMse{0.0f};
static std::atomic<long long> gLastWindowStartNs{-1};

extern "C"
JNIEXPORT jboolean JNICALL
Java_com_example_real_1time_1vad_MainActivity_nativeDequeueWindow(
        JNIEnv* env, jobject /*thiz*/, jfloatArray outArr) {

    if (!gPipe) return JNI_FALSE;

        std::vector<float> win;
    long long windowStartNs = -1;
    if (!gPipe->popFeatureWindow(win, &windowStartNs)) return JNI_FALSE;

    const jsize outN = env->GetArrayLength(outArr);
    if (outN != (jsize)win.size()) return JNI_FALSE;

    env->SetFloatArrayRegion(outArr, 0, outN, win.data());
    gLastWindowStartNs.store(windowStartNs, std::memory_order_relaxed);
    return JNI_TRUE;
}

extern "C"
JNIEXPORT void JNICALL
Java_com_example_real_1time_1vad_MainActivity_nativeSubmitMSE(
        JNIEnv* /*env*/, jobject /*thiz*/, jfloat mse) {
    if (!gPipe) return;
    gPipe->submitMSE((float)mse);
}

extern "C"
JNIEXPORT jfloat JNICALL
Java_com_example_real_1time_1vad_MainActivity_nativeGetThreshold(
        JNIEnv* /*env*/, jobject /*thiz*/) {
    if (!gPipe) return 0.0f;
    //return (jfloat)gPipe->mseThr.load(std::memory_order_relaxed);
    return (jfloat)gPipe->getMseThreshold();
}

extern "C"
JNIEXPORT jboolean JNICALL
Java_com_example_real_1time_1vad_MainActivity_nativeStartFileMode(
        JNIEnv* env, jobject /*thiz*/, jstring path, jint bufferSize, jint sampleRate) {
    if (!gPipe) gPipe = new AudioPipeline(AudioConfig::SR * 5);
    gPipe->start();

    if (gHasStats.load(std::memory_order_relaxed) && gPipe) {
        gPipe->setTrainStats(
                gMn.load(), gMx.load(),
                gMseThr.load()
        );
    }
    if (!path) return JNI_FALSE;
    const char* cpath = env->GetStringUTFChars(path, nullptr);
    if (!cpath) return JNI_FALSE;
    LOGI("nativeStartFileMode path=%s sr=%d buf=%d", cpath, (int)sampleRate, (int)bufferSize);
    bool ok = SuperpoweredIO_StartFileMode(gPipe, cpath, (int)bufferSize, (int)sampleRate);
    env->ReleaseStringUTFChars(path, cpath);
    LOGI("nativeStartFileMode ok=%d", ok ? 1 : 0);
    return ok ? JNI_TRUE : JNI_FALSE;
}
extern "C"
JNIEXPORT void JNICALL
Java_com_example_real_1time_1vad_MainActivity_nativeSetFileMonitor(JNIEnv* env, jobject /*thiz*/, jboolean enabled) {
    (void)env;
    SuperpoweredIO_SetFileMonitor(enabled == JNI_TRUE);
}

extern "C"
JNIEXPORT jboolean JNICALL
Java_com_example_real_1time_1vad_MainActivity_nativeIsFilePlaying(JNIEnv* env, jobject /*thiz*/) {
    (void)env;
    return SuperpoweredIO_IsFilePlaying() ? JNI_TRUE : JNI_FALSE;
}

extern "C"
JNIEXPORT jboolean JNICALL
Java_com_example_real_1time_1vad_MainActivity_nativePush16kChunk(
        JNIEnv* env, jobject /*thiz*/, jfloatArray pcm16k, jint count) {
    if (!gPipe || !pcm16k) return JNI_FALSE;
    const jsize len = env->GetArrayLength(pcm16k);
    if (len <= 0) return JNI_FALSE;
    int n = (int)len;
    if (count > 0 && count < n) n = count;

    jboolean isCopy = JNI_FALSE;
    jfloat* data = env->GetFloatArrayElements(pcm16k, &isCopy);
    if (!data) return JNI_FALSE;
    gPipe->push16kMono((const float*)data, n);
    env->ReleaseFloatArrayElements(pcm16k, data, JNI_ABORT);
    return JNI_TRUE;
}

extern "C"
JNIEXPORT jint JNICALL
Java_com_example_real_1time_1vad_MainActivity_nativeGetConfigSr(
        JNIEnv* /*env*/, jobject /*thiz*/) {
    return (jint)AudioConfig::SR;
}

extern "C"
JNIEXPORT jint JNICALL
Java_com_example_real_1time_1vad_MainActivity_nativeGetConfigHop(
        JNIEnv* /*env*/, jobject /*thiz*/) {
    return (jint)AudioConfig::HOP;
}

extern "C"
JNIEXPORT jint JNICALL
Java_com_example_real_1time_1vad_MainActivity_nativeGetConfigSlideW(
        JNIEnv* /*env*/, jobject /*thiz*/) {
    return (jint)AudioConfig::SLIDE_W;
}

extern "C"
JNIEXPORT jfloat JNICALL
Java_com_example_real_1time_1vad_MainActivity_nativeGetProcessingTimeMs(
        JNIEnv* /*env*/, jobject /*thiz*/) {
    return (jfloat)SuperpoweredIO_GetAvgCallbackMs();
}

extern "C"
JNIEXPORT jfloat JNICALL
Java_com_example_real_1time_1vad_MainActivity_nativeGetMicRms(
        JNIEnv* /*env*/, jobject /*thiz*/) {
    return (jfloat)SuperpoweredIO_GetMicRms();
}

extern "C"
JNIEXPORT jfloat JNICALL
Java_com_example_real_1time_1vad_MainActivity_nativeGetFeatureProcessingTimeMs(
        JNIEnv* /*env*/, jobject /*thiz*/) {
    if (!gPipe) return 0.0f;
    return (jfloat)gPipe->getAvgFeatureMs();
}

extern "C"
JNIEXPORT jfloat JNICALL
Java_com_example_real_1time_1vad_MainActivity_nativeGetFrameProcessingTimeMs(
        JNIEnv* /*env*/, jobject /*thiz*/) {
    float cb = SuperpoweredIO_GetAvgCallbackMs();
    float feat = gPipe ? gPipe->getAvgFeatureMs() : 0.0f;
    return (jfloat)(cb + feat);
}



extern "C"
JNIEXPORT void JNICALL
Java_com_example_real_1time_1vad_MainActivity_nativeSetTrainStats(
        JNIEnv* env, jobject thiz,
        jfloat mn, jfloat mx,
        jfloat mseThreshold) {

    (void)env; (void)thiz;

    // Keep your globals if you want (optional)
    gMn.store(mn);
    gMx.store(mx);
    gMseThr.store(mseThreshold);
    gHasStats.store(true);

    // ✅ THIS is where to do it:
    if (gPipe) {
        gPipe->setTrainStats(
                mn, mx,
                mseThreshold
        );
        LOGI("Train stats forwarded to AudioPipeline.");
    } else {
        LOGI("Train stats saved, but gPipe is null (call nativeStart first or buffer stats).");
    }

    LOGI("Train stats set. mseThr=%f", mseThreshold);

}

extern "C"
JNIEXPORT jint JNICALL
Java_com_example_real_1time_1vad_MainActivity_nativeGetLatestDecision(JNIEnv* env, jobject thiz) {
    (void)env; (void)thiz;
    if (!gPipe) return -1;
    int d; float mse;
    if (!gPipe->getLatestDecision(d, mse)) return -1;
    return (jint)d;
}

extern "C"
JNIEXPORT jfloat JNICALL
Java_com_example_real_1time_1vad_MainActivity_nativeGetLatestMse(JNIEnv* env, jobject thiz) {
    (void)env; (void)thiz;
    if (!gPipe) return 0.0f;
    int d; float mse;
    if (!gPipe->getLatestDecision(d, mse)) return 0.0f;
    return (jfloat)mse;
}



extern "C"
JNIEXPORT jlong JNICALL
Java_com_example_real_1time_1vad_MainActivity_nativeGetLastWindowStartNs(
        JNIEnv* /*env*/, jobject /*thiz*/) {
    return (jlong)gLastWindowStartNs.load(std::memory_order_relaxed);
}

extern "C"
JNIEXPORT void JNICALL
Java_com_example_real_1time_1vad_MainActivity_nativeStart(JNIEnv *env, jobject thiz,
                                                          jint sampleRate, jint bufferSize) {
    (void)env; (void)thiz;

    // 1) create pipeline
    if (!gPipe) gPipe = new AudioPipeline(AudioConfig::SR * 5);
    gPipe->start();

    // 2) start Superpowered (same as you already do)
    LOGI("nativeStart sr=%d buf=%d", (int)sampleRate, (int)bufferSize);
    if (!SuperpoweredIO_Start(gPipe, (int)sampleRate, (int)bufferSize)) {
        LOGE("Superpowered start failed");
    }
    LOGI("Superpowered started + AudioPipeline started");
    if (gHasStats.load(std::memory_order_relaxed) && gPipe) {
        gPipe->setTrainStats(
                gMn.load(), gMx.load(),
                gMseThr.load()
        );
        LOGI("Applied cached train stats to AudioPipeline at nativeStart().");
    }
}

extern "C"
JNIEXPORT void JNICALL
Java_com_example_real_1time_1vad_MainActivity_nativeStartPipelineOnly(JNIEnv* env, jobject thiz) {
    (void)env; (void)thiz;

    if (!gPipe) gPipe = new AudioPipeline(AudioConfig::SR * 5);
    gPipe->start();

    if (gHasStats.load(std::memory_order_relaxed) && gPipe) {
        gPipe->setTrainStats(
                gMn.load(), gMx.load(),
                gMseThr.load()
        );
        LOGI("Applied cached train stats to AudioPipeline at nativeStartPipelineOnly().");
    }
    LOGI("nativeStartPipelineOnly done");
}

extern "C"
JNIEXPORT void JNICALL
Java_com_example_real_1time_1vad_MainActivity_nativeStop(JNIEnv *env, jobject thiz) {
    (void)env; (void)thiz;

    SuperpoweredIO_Stop();

    if (gPipe) {
        gPipe->stop();
        delete gPipe;
        gPipe = nullptr;
    }

    LOGI("Superpowered stopped + AudioPipeline stopped");
}

extern "C"
JNIEXPORT jboolean JNICALL
Java_com_example_real_1time_1vad_MainActivity_nativeGetFeatures(JNIEnv *env, jobject thiz,
                                                                jfloatArray out) {
    (void)thiz;
    if (!out || !gPipe) return JNI_FALSE;

    jsize len = env->GetArrayLength(out);
    if (len < (jsize)FeatureFrame::DIM) return JNI_FALSE;

    FeatureFrame local;
    if (!gPipe->getLatestFeatures(local)) return JNI_FALSE;

    env->SetFloatArrayRegion(out, 0, FeatureFrame::DIM, local.feat.data());
    return JNI_TRUE;
}

