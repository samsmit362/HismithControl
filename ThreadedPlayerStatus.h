#pragma once

#include <QString>
#include <windows.h>

#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>

class ThreadedPlayerStatus;

// Snapshot of the video player status produced by the background poller.
// Mirrors the out-parameters used by make_video_player_status / run_funscript.
struct VideoPlayerStatus
{
    bool is_valid = false;
    bool is_paused = true;
    QString video_filepath;
    int video_pos = -1;
    __int64 sys_time = -1;
    double rate = 1.0;
    LARGE_INTEGER poll_time = {};
};

extern ThreadedPlayerStatus g_threaded_player_status;
extern VideoPlayerStatus g_video_player_status;

// Background worker that polls the video player status on a fixed period
// (default 4 Hz / 250 ms) so that the run_funscript tracking loop can read the
// latest cached state in O(1) without ever blocking on the network request.
class ThreadedPlayerStatus
{
public:
    bool m_is_running{ false };

private:
    std::thread m_worker_thread;
    std::atomic<bool> m_stop_requested{ false };
    std::mutex m_stop_mutex;
    std::condition_variable m_stop_cv;

    unsigned int m_poll_interval_ms = 250;

    mutable std::mutex m_state_mutex;
    mutable std::mutex m_make_mutex;
    VideoPlayerStatus m_cached_state;

    void status_loop();

public:
    ThreadedPlayerStatus() = default;
    ~ThreadedPlayerStatus() { stop(); }

    ThreadedPlayerStatus(const ThreadedPlayerStatus&) = delete;
    ThreadedPlayerStatus& operator=(const ThreadedPlayerStatus&) = delete;

    // If initial_state.is_valid is true, it is used as the first cached state.
    // Otherwise a blocking make_video_player_status() call is performed before
    // the worker thread is started.
    void start(VideoPlayerStatus& initial_state, unsigned int poll_interval_ms = 250);
    void stop();
    
    VideoPlayerStatus make_video_player_status(VideoPlayerCommand command = VideoPlayerCommand::None);

    // O(1) copy of the latest cached status; never performs a network call.
    VideoPlayerStatus getLatestState() const;
};
