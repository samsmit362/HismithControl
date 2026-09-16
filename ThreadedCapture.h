#pragma once

#include "HighPrecisionTimerGuard.h"

#include <QThread>
#include <opencv2/core.hpp>
#include <opencv2/highgui.hpp>
#include <deque>

class ThreadedCapture;

extern ThreadedCapture g_threaded_capture;

class QThread;

class ThreadedCapture
{
public:
    bool is_running{ false };

    __int64 get_skipped_frames();
    __int64 get_bad_frames();
    void clean_counters();

private:
    __int64 producer_total{ 0 };
    __int64 consumer_total{ 0 };
    __int64 bad_total{ 0 };

private:
    std::thread capture_thread;

    std::mutex cap_mutex;
    std::condition_variable cvar;

    cv::VideoCapture* p_cap{ NULL };

    // buffer_size == 1 -> legacy single-slot path (latest_frame), exactly as before.
    // buffer_size > 1 -> FIFO ring buffer (frame_queue) of that capacity.
    int buffer_size{ 1 };

    // Legacy single-slot fields (used only when buffer_size == 1).
    cv::Mat latest_frame;
    LARGE_INTEGER m_frame_read_time;
    __int64 msec_pos_latest_frame;
    bool has_new_frame{ false };

    // FIFO queue (used only when buffer_size > 1).
    // Each element is (frame, timestamp_ms, frame_read_time).
    struct QueuedFrame {
        cv::Mat frame;
        __int64 msec;
        LARGE_INTEGER read_time;
    };
    std::deque<QueuedFrame> frame_queue;

    void capture_loop();

public:
    ThreadedCapture() = default;

    ~ThreadedCapture() {
        stop();
    }

    // buffer_size == 1 (default) -> single-slot fast path (legacy behaviour, maximum speed).
    // buffer_size  > 1           -> FIFO ring buffer with the given capacity; consumer
    //                               receives ALL accumulated frames in order, so a slower
    //                               consumer does not lose frames, and dt between
    //                               consecutive consumer reads is always a single frame.
    void start(cv::VideoCapture* p_capture, int buffer_size = 1);
    void stop();
    bool wait_and_get_fresh_frame(cv::Mat& output_frame, __int64& msec_pos_output_frame, LARGE_INTEGER& frame_read_time);
};
