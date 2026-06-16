//
// Created by nafis on 2/16/2026.
//

#include "FeatureExtractor.h"
#include <cmath>
#include <algorithm>
#include "AudioConfig.h"
#include <cstring>

// If you have Superpowered FFT available, use it.
// Otherwise for now we just stub FFT out and focus on framing.
// Replace computePowerSpectrum() with real FFT next.

// ----------------------------
// Helpers
// ----------------------------
static inline float clampf(float x, float lo, float hi) {
    return (x < lo) ? lo : (x > hi) ? hi : x;
}

float FeatureExtractor::hzToMel(float hz) {
    return 2595.0f * std::log10(1.0f + hz / 700.0f);
}
float FeatureExtractor::melToHz(float mel) {
    return 700.0f * (std::pow(10.0f, mel / 2595.0f) - 1.0f);
}

// ----------------------------
// Constructor
// ----------------------------


FeatureExtractor::FeatureExtractor()
        : winBuf(WIN, 0.0f),
          hann(WIN, 0.0f),
          melFb(N_MEL * NBINS, 0.0f),
          dct(N_MFCC * N_MEL, 0.0f),
          fftIn(NFFT, 0.0f),
          mag(NBINS, 0.0f),
          powSpec(NBINS, 0.0f),
          melE(N_MEL, 0.0f),
          mfcc(N_MFCC, 0.0f)
{
    buildHann();
    buildFftTables();
    buildMelFilterbank();
    buildDct();
}

// ----------------------------
// Streaming entrypoint
// ----------------------------
// Feed HOP samples each call. When enough samples have accumulated to form a WIN,
// we compute one feature frame.

bool FeatureExtractor::processHop(const float* hopSamples, FeatureFrame& out) {
        //if (n != HOP) return false;
    // shift left by HOP, append new hop at end (streaming window)
        if (HOP > WIN) return false;

        // memmove window left
        std::memmove(winBuf.data(), winBuf.data() + HOP, (WIN - HOP) * sizeof(float));
        // append
        std::memcpy(winBuf.data() + (WIN - HOP), hopSamples, HOP * sizeof(float));

        // windowed -> fftIn (zero-pad to NFFT)
        for (int i = 0; i < WIN; i++) fftIn[i] = winBuf[i] * hann[i];
        for (int i = WIN; i < NFFT; i++) fftIn[i] = 0.0f;

        // FFT

        //Your code starts here

    fft_real_pow(fftIn.data(), powSpec.data());
        //Your code ends here



        computeMfccFromPow(powSpec.data(), mfcc.data());


        // pack output: MFCC-only uses first 80.
        for (int i = 0; i < N_MFCC; i++) out.feat[i] = mfcc[i];


        return true;
    }

void FeatureExtractor::buildFftTables() {
    const float pi = 3.14159265359f;

    // Safety: ensure NFFT is power of two
    static_assert((NFFT & (NFFT - 1)) == 0, "NFFT must be power of 2");

    // twiddles for base N=512: W512^k = cos(-2pi k/512) + j sin(-2pi k/512)
    for (int k = 0; k < NFFT/2; k++) {
        float ang = -2.0f * pi * k / float(NFFT);
        twCos[k] = std::cos(ang);
        twSin[k] = std::sin(ang);
    }

    // Compute number of bits = log2(NFFT)
    int bits = 0;
    int tmp = NFFT;
    while (tmp > 1) {
        tmp >>= 1;
        bits++;
    }

    // bit reversal table for 512 (9 bits)
    for (int i = 0; i < NFFT; i++) {
        uint16_t x = (uint16_t)i;
        uint16_t r = 0;
        for (int b = 0; b < bits; b++) { r = (r << 1) | (x & 1); x >>= 1; }
        bitrev[i] = r;
    }

    fftTablesReady = true;
}



// ----------------------------
// Precompute window
// ----------------------------
void FeatureExtractor::buildHann() {

    //Your code starts here
    const float pi = 3.14159265359f;

    for (int n = 0; n < WIN; n++) {
        hann[n] = 0.5f - 0.5f * std::cos((2.0f * pi * n) / float(WIN - 1));
    }

    //Your code ends here
}

// ----------------------------
// Mel filterbank (triangular)
// ----------------------------
void FeatureExtractor::buildMelFilterbank() {
    // Typical: fmin=0, fmax=sr/2
    float fMin = 0.0f;
    float fMax = 0.5f * SR;

    float melMin = hzToMel(fMin);
    float melMax = hzToMel(fMax);

    std::vector<float> melPts(N_MEL + 2);
    for (int i = 0; i < N_MEL + 2; i++) {
        float t = float(i) / float(N_MEL + 1);
        melPts[i] = melMin + t * (melMax - melMin);
    }

    std::vector<float> hzPts(N_MEL + 2);
    for (int i = 0; i < N_MEL + 2; i++) hzPts[i] = melToHz(melPts[i]);

    std::vector<int> bin(N_MEL + 2);
    for (int i = 0; i < N_MEL + 2; i++) {
        // map hz -> FFT bin index
        float b = (hzPts[i] / fMax) * (NBINS - 1);
        bin[i] = (int)std::floor(clampf(b, 0.0f, float(NBINS - 1)));
    }

    std::fill(melFb.begin(), melFb.end(), 0.0f);

    for (int m = 0; m < N_MEL; m++) {
        int left = bin[m];
        int center = bin[m + 1];
        int right = bin[m + 2];


        if (center <= left)   center = left + 1;
        if (right  <= center) right  = center + 1;

        for (int k = left; k < center && k < NBINS; k++) {
            float w = float(k - left) / float(center - left);
            melFb[m * NBINS + k] = w;
        }
        for (int k = center; k < right && k < NBINS; k++) {
            float w = float(right - k) / float(right - center);
            melFb[m * NBINS + k] = w;
        }
        // Slaney normalization
        float denom = hzPts[m + 2] - hzPts[m];
        if (denom > 1e-12f) {
            float enorm = 2.0f / denom;
            for (int k = 0; k < NBINS; k++) {
                melFb[m * NBINS + k] *= enorm;
            }
        }
    }
}

// ----------------------------
// DCT-II matrix for MFCC
// ----------------------------
void FeatureExtractor::buildDct() {


    //Your code starts here
    const float pi = 3.14159265359f;

    for (int i = 0; i < N_MFCC; i++) {
        for (int j = 0; j < N_MEL; j++) {
            dct[i * N_MEL + j] = std::cos((pi * i * (j + 0.5f)) / float(N_MEL));
        }
    }

    //Your code ends here
}

// ----------------------------
// MFCC from power spectrum
// ----------------------------
void FeatureExtractor::computeMfccFromPow(const float* pow, float* outMfcc) {
    // mel energies
    const float eps = 1e-10f;
    for (int m = 0; m < N_MEL; m++) {
        double acc = 0.0;
        const float* row = &melFb[m * NBINS];
        for (int k = 0; k < NBINS; k++) {
            acc += double(row[k]) * double(pow[k]);
        }
        // log energy
        //melE[m] = std::logf((float)acc + eps);
        melE[m] = 10.0f * std::log10(std::max((float)acc, eps));
    }

    // DCT
    for (int i = 0; i < N_MFCC; i++) {
        double acc = 0.0;
        const float* row = &dct[i * N_MEL];
        for (int j = 0; j < N_MEL; j++) {
            acc += double(row[j]) * double(melE[j]);
        }

        outMfcc[i] = 2.0f * (float)acc;
    }


}

void FeatureExtractor::fft_real_pow(const float* x, float* outPow) {


        //Your code starts here
        if (!fftTablesReady) buildFftTables();

        std::vector<float> re(NFFT, 0.0f);
        std::vector<float> im(NFFT, 0.0f);

        // Bit-reversal reorder: real input, imag = 0
        for (int i = 0; i < NFFT; i++) {
            re[bitrev[i]] = x[i];
            im[bitrev[i]] = 0.0f;
        }

        // Iterative radix-2 Cooley-Tukey FFT
        for (int len = 2; len <= NFFT; len <<= 1) {
            int halfLen = len >> 1;
            int twStep = NFFT / len;

            for (int start = 0; start < NFFT; start += len) {
                for (int j = 0; j < halfLen; j++) {
                    int even = start + j;
                    int odd  = even + halfLen;

                    int twIdx = j * twStep;
                    float wr = twCos[twIdx];
                    float wi = twSin[twIdx];

                    float orr = re[odd];
                    float oii = im[odd];

                    // t = W * odd
                    float tr = wr * orr - wi * oii;
                    float ti = wr * oii + wi * orr;

                    float er = re[even];
                    float ei = im[even];

                    re[even] = er + tr;
                    im[even] = ei + ti;
                    re[odd]  = er - tr;
                    im[odd]  = ei - ti;
                }
            }
        }

        // Power spectrum for real signal: keep non-redundant bins only
        for (int k = 0; k < NBINS; k++) {
            outPow[k] = re[k] * re[k] + im[k] * im[k];
        }

        //Your code ends here
    }



