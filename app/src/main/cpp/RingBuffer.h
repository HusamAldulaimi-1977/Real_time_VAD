//
// Created by nafis on 2/15/2026.
//

#pragma once
#include <vector>
#include <cstddef>
#include <algorithm>

class RingBuffer {
public:
    explicit RingBuffer(size_t capacity)
            : buffer(capacity + 1),  // +1 to distinguish full vs empty
              capacity(capacity + 1),
              readIndex(0),
              writeIndex(0) {}

    size_t availableToRead() const {
        return (writeIndex + capacity - readIndex) % capacity;
    }

    size_t availableToWrite() const {
        return capacity - 1 - availableToRead();
    }

    // Push up to n samples. Returns number actually pushed.
    size_t push(const float* input, size_t n) {//write input samples into the buffer
        size_t toWrite = std::min(n, availableToWrite());


        //Your code starts here
        for (size_t i = 0; i < toWrite; i++) {
            buffer[writeIndex] = input[i];
            writeIndex = (writeIndex + 1) % capacity;//
        }


        //Your code ends here

        return toWrite;
    }

    // Pop exactly n samples. Returns false if not enough available.
    bool pop(float* output, size_t n) {//how many samples you want to read from the buffer
        if (availableToRead() < n) return false;//Do we have enough samples to read n elements


        //Your code starts here
        for (size_t i = 0; i < n; i++) {
            output[i] = buffer[readIndex];
            readIndex = (readIndex + 1) % capacity;
        }

        //Your code ends here

        return true;
    }

    void reset() {
        readIndex = 0;
        writeIndex = 0;
    }

private:
    std::vector<float> buffer;
    const size_t capacity;
    size_t readIndex;
    size_t writeIndex;
};
