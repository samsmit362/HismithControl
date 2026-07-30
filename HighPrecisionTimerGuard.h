#pragma once

#include <windows.h>
#include <timeapi.h>

class HighPrecisionTimerGuard;

extern HighPrecisionTimerGuard g_high_precision_timer_guard;

class HighPrecisionTimerGuard {
public:
    bool was_set;

    HighPrecisionTimerGuard() {
        was_set = false;
    }

    void Start() {
        if (!was_set)
        {
            was_set = true;
            timeBeginPeriod(1);
        }
    }

    void Stop() {
        if (was_set)
        {
            was_set = false;
            timeEndPeriod(1);
        }
    }

    ~HighPrecisionTimerGuard() {
    }

    HighPrecisionTimerGuard(const HighPrecisionTimerGuard&) = delete;
    HighPrecisionTimerGuard& operator=(const HighPrecisionTimerGuard&) = delete;
};
