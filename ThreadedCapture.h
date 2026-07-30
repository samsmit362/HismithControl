#pragma once

#include "HighPrecisionTimerGuard.h"

#include <QThread>
#include <opencv2/core.hpp>
#include <opencv2/highgui.hpp>

class ThreadedCapture;

extern ThreadedCapture g_threaded_capture;

class ThreadedCapture
{
public:
    bool is_running{ false };

private:
    std::thread capture_thread;

    std::mutex cap_mutex;
    std::condition_variable cvar;

    cv::VideoCapture* p_cap{ NULL};
    cv::Mat latest_frame;
    __int64 msec_pos_latest_frame;
    bool has_new_frame{ false };

    void capture_loop();

public:
    ThreadedCapture() = default;

    ~ThreadedCapture() {
        stop();
    }

    void start(cv::VideoCapture* p_capture);
    void stop();
    bool wait_and_get_fresh_frame(cv::Mat& output_frame, __int64& msec_pos_output_frame);
};
