//
//  FIRFilter.h
//  NC-2Ch
//
//  Created by nafis on 2/16/2026.
//

#ifndef FIRFilter_h
#define FIRFilter_h

#include <stdio.h>
#include <stdlib.h>



typedef struct FIR {
    
    int N;//number of samples you process each time
    
    float* inputBuffer;//a memory array that stores samples
} FIR;

FIR* initFIR(int stepSize);
void processFIRFilter(FIR* fir, float* input, float* output);
void destroyFIR(FIR **fir);

#endif /* FIRFilter_h */
