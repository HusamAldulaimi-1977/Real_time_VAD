//
// Created by nafis on 2/16/2026.
//

#pragma once
#include <vector>
#include <cstddef>
#include <cstdint>
#include <array>
#include "AudioConfig.h"

struct FeatureFrame {
    static constexpr int DIM =
            AudioConfig::C_USED;

    std::array<float, DIM> feat{};
};

class FeatureExtractor {
public:
    FeatureExtractor();

    bool processHop(const float* hopSamples, FeatureFrame& out);


private:
    static constexpr int SR    = AudioConfig::SR;
    static constexpr int WIN   = AudioConfig::WIN;
    static constexpr int HOP   = AudioConfig::HOP;
    static constexpr int NFFT  = AudioConfig::NFFT;
    static constexpr int NBINS = AudioConfig::N_BINS;

    static constexpr int N_MFCC = AudioConfig::N_MFCC;


    static constexpr int N_MEL  = 128; // internal mel bins
    static float hzToMel(float hz);
    static float melToHz(float mel);

    std::vector<float> winBuf;
    std::vector<float> hann;
    std::vector<float> melFb;
    std::vector<float> dct;

    std::vector<float> fftIn;
    std::vector<float> mag;
    std::vector<float> powSpec;
    std::vector<float> melE;
    std::vector<float> mfcc;



    std::array<float, 256> twCos{};
    std::array<float, 256> twSin{};
    std::array<uint16_t, 512> bitrev{};
    bool fftTablesReady = false;



private:
    void buildHann();
    void buildMelFilterbank();
    void buildDct();

    void buildFftTables();
    void fft_real_pow(const float* x, float* outPow); // outPow size NBINS

    void computeMfccFromPow(const float* pow, float* outMfcc);


};


