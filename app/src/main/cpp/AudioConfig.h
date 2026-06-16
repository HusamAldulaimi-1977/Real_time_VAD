//
// Created by nafis on 2/16/2026.
//

#pragma once

namespace AudioConfig {

// Audio I/O (device side)
    static constexpr int IO_SR = 0; // 0 = device native samplerate
    static constexpr int IO_BUFFER_SIZE = 0; // 0 = device preferred buffer size
    static constexpr int DECIMATION_FACTOR = 3; // IO_SR / SR

// Audio sample rate AFTER resampling
    static constexpr int SR = 16000;

// Framing
    static constexpr int HOP = 160;      // 10 ms
    static constexpr int WIN = 400;      // 25 ms
    static constexpr int NFFT = 512;     // FFT size
    static constexpr int N_BINS = NFFT / 2 + 1; // 257

// Feature dimensions
    static constexpr int N_MFCC = 80;
    static constexpr int C_USED = N_MFCC;

    static constexpr int FRAME_SIZE  = 480;
    static constexpr int SLIDE_W     = 64;    // frames per window
    static constexpr int SLIDE_STRIDE = 8;

}


