//
//  FIRFilter.c
//  NC-2Ch
//
//  Created by nafis on 2/16/2026.
//
//

#include "FIRFilter.h"
#include "filterCoefficients.h"

float checkRange(float input){

    //Your code starts here
    if (input > 1.0f) {
        return 1.0f;
    } else if (input < -1.0f) {
        return -1.0f;
    } else {
        return input;
    }

    //Your code ends here
}

FIR* initFIR(int stepSize) {
    
    FIR* fir = (FIR*)malloc(sizeof(FIR));
    
    fir->N = stepSize;
    
    fir->inputBuffer = (float*)calloc(2*stepSize, sizeof(float));
    
    return fir;
    
}

void processFIRFilter(FIR* fir, float* input, float* output) {
    

    //Your code starts here
    int M = NCOEFFS - 1; // Number of previous samples needed (80)

    for (int n = 0; n < fir->N; n++) {
        float y = 0.0f;

        for (int k = 0; k < NCOEFFS; k++) {
            int sampleIdx = n - k;
            float currentSample;

            if (sampleIdx >= 0) {
                // The sample is within the current input block
                currentSample = input[sampleIdx];
            } else {
                // The sample is from the previous block (stored in history)
                // If sampleIdx is -1, we take the most recent history sample
                currentSample = fir->inputBuffer[M + sampleIdx];
            }

            y += currentSample * filterCoefficients[k];
        }

        // Apply clipping/range check before storing to output
        output[n] = checkRange(y);
    }

    // --- UPDATE HISTORY FOR THE NEXT BLOCK ---
    // We need to save the last M (80) samples seen in this block
    if (fir->N >= M) {
        // Standard case: Block size is larger than filter history
        for (int k = 0; k < M; k++) {
            fir->inputBuffer[k] = input[fir->N - M + k];
        }
    } else {
        // Edge case: Block size is very small, shift old history and append new
        for (int k = 0; k < M - fir->N; k++) {
            fir->inputBuffer[k] = fir->inputBuffer[k + fir->N];
        }
        for (int k = 0; k < fir->N; k++) {
            fir->inputBuffer[M - fir->N + k] = input[k];
        }
    }

    //Your code ends here
}

void destroyFIR(FIR **fir) {
    
    if ((*fir) != NULL) {
        
        if ((*fir)->inputBuffer != NULL) {
            free((*fir)->inputBuffer);
            (*fir)->inputBuffer = NULL;
        }
        
        free((*fir));
        (*fir) = NULL;        
    }
    
}
