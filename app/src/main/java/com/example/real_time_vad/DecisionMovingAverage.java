package com.example.real_time_vad;

import java.util.ArrayDeque;

public class DecisionMovingAverage {
    private final int windowSize;
    private final ArrayDeque<Integer> history = new ArrayDeque<>();
    private int sum = 0;

    public DecisionMovingAverage(int windowSize) {
        this.windowSize = Math.max(1, windowSize);
    }

    public int update(int decision) {
        if (decision != 0 && decision != 1) return decision;
        history.addLast(decision);
        sum += decision;
        if (history.size() > windowSize) {
            Integer removed = history.removeFirst();
            if (removed != null) sum -= removed;
        }
        return (sum / (float) history.size()) >= 0.5f ? 1 : 0;
    }

    public void reset() {
        history.clear();
        sum = 0;
    }
}
