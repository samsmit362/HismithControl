#pragma once

#include <QString>
#include <windows.h>

#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>

class QNetworkAccessManager;
class QNetworkRequest;
class HereSphereSync;

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
//
// Thread-affinity policy:
//  - The background poller runs in a private std::thread (m_worker_thread).
//  - Any call that performs a network request (make_video_player_status, or
//    the poller's status_loop) allocates a QNetworkAccessManager and
//    QNetworkRequest bound to the calling thread.  Every QObject child
//    (QNetworkReply, internal timers) therefore stays in a single thread,
//    which eliminates the "QObject: Cannot create children for a parent that
//    is in a different thread" and "QObject::killTimer: Timers cannot be
//    stopped from another thread" warnings that the previous global
//    "QNetworkAccessManager + deleteLater" pattern produced.
//  - For HereSphere players the HereSphereSync socket object is owned by this
//    class and lives/starts/stops in the calling thread that invoked
//    connect_to_player() / disconnect_from_player().
class ThreadedPlayerStatus
{
public:
    bool m_is_running{ false };

private:
    std::thread m_worker_thread;
    std::atomic<bool> m_stop_requested{ false };
    std::mutex m_stop_mutex;
    std::condition_variable m_stop_cv;

    QNetworkAccessManager* m_p_nma = nullptr;
    QNetworkRequest       *m_p_req = nullptr;

    unsigned int m_poll_interval_ms = 250;

    mutable std::mutex m_state_mutex;
    mutable std::mutex m_make_mutex;
    VideoPlayerStatus m_cached_state;

    // True once connect_to_player() has succeeded for the current run_funscript
    // session.  Used by disconnect_from_player() to avoid double-freeing the
    // HereSphereSync instance (which is only meaningful for HereSphere
    // players and is nulled on every disconnect_from_player()).
    bool m_player_connected = false;
    HereSphereSync* m_heresphere = nullptr;

    // Helpers
    void status_loop();
    VideoPlayerStatus do_fetch(QNetworkAccessManager* nma,
                               QNetworkRequest* req,
                               VideoPlayerCommand command) const;

public:
    ThreadedPlayerStatus() = default;
    ~ThreadedPlayerStatus() { stop(); disconnect_from_player(); }

    ThreadedPlayerStatus(const ThreadedPlayerStatus&) = delete;
    ThreadedPlayerStatus& operator=(const ThreadedPlayerStatus&) = delete;

    // Open the connection / session to the currently selected video player
    // type.  Reads g_video_player_type and the URL/port/password globals.
    // For VLC and RVP this only configures the request template (auth
    // header, timeout); for HereSphere it launches the socket session.
    void connect_to_player();

    // Inverse of connect_to_player().  Safe to call multiple times or without a
    // prior connect_to_player().
    void disconnect_from_player();

    // If initial_state.is_valid is true, it is used as the first cached state.
    // Otherwise a blocking network call is performed before the worker thread
    // is started.
    void start(VideoPlayerStatus& initial_state, unsigned int poll_interval_ms = 250);
    void stop();

    VideoPlayerStatus make_video_player_status(VideoPlayerCommand command = VideoPlayerCommand::None);

    // O(1) copy of the latest cached status; never performs a network call.
    VideoPlayerStatus getLatestState() const;

    // Expose the HereSphereSync instance (nullptr for VLC / RVP / UNKNOWN)
    // so that make_here_sphere_status_request can read the latest frame
    // without duplicating the ownership logic.
    HereSphereSync* heresphere() const { return m_heresphere; }
};
