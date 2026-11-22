// WinRtBleManager.h
//
// Production-ready WinRT Bluetooth LE client for Hismith / Wildolo / Fredorch
// hardware. Provides:
//   - Synchronous, non-blocking-by-design `findHismithDevices` (scan WITHOUT
//     a persistent connection — devices stay unheld for Intiface).
//   - Synchronous `connectToDevice` (blocks the caller until handshake or
//     timeout; thread-safe; releases half-bound GATT reference on failure).
//   - Low-latency `sendSpeedCommand` via WriteWithoutResponse, with
//     LATEST-WINS coalescing (a new speed cancels any pending one, so the
//     device is always commanded at the freshest value).
//   - Idempotent `disconnectDevice` that drains in-flight writes by pumping
//     the Windows message loop (WinRT Completed delegates fire on this
//     thread, so the drain actually completes instead of "abandoning").
//
// Thread-safety: all mutable state is guarded by `m_mtx`; `m_connected` is
// an atomic. The `QMetaObject::invokeMethod` dance from the draft is gone —
// sync wait via `std::promise` + `IAsyncAction::Completed`.

#pragma once

#include <QObject>
#include <QString>
#include <vector>
#include <memory>
#include <mutex>
#include <atomic>
#include <functional>

enum class DeviceProtocolMode {
    Unknown,
    LegacyHismith,
    HismithMini,
    FredorchF21S
};

// Result of a BLE advertisement scan. Returned by findHismithDevices().
struct BleDeviceInfo {
    QString name;          // advertisement local name (human readable)
    uint64_t address = 0;  // Bluetooth LE MAC address (for connectToDevice)
};

class WinRtBleManager : public QObject {
    Q_OBJECT
public:
    explicit WinRtBleManager(QObject* parent = nullptr);
    ~WinRtBleManager();

    struct Impl;

    // -----------------------------------------------------------------
    // Shared state — kept public so the free-function coroutine
    // connectHandshakeImpl() in WinRtBleManager.cpp can read/write
    // it without a friend declaration (which would leak winrt types
    // into this header).
    // -----------------------------------------------------------------
    mutable std::mutex    m_mtx;
    std::atomic<bool>     m_connected { false };
    DeviceProtocolMode    m_protocolMode = DeviceProtocolMode::Unknown; // guarded by m_mtx
    QString               m_lastError;   // guarded by m_mtx — diagnostic detail

    // -----------------------------------------------------------------
    // PHASE 1: Synchronous advertisement scan. Does NOT open a persistent
    // GATT connection — devices are NOT held.
    //   timeout_ms: how long to listen (ms). Use 3000-5000 for a "refresh"
    //               UI call, 1000-1500 for a quick probe in tests.
    //   Returns a deduped list of Hismith/Wildolo/Fredorch devices seen.
    // -----------------------------------------------------------------
    std::vector<BleDeviceInfo> findHismithDevices(int timeout_ms = 10000);

    // -----------------------------------------------------------------
    // PHASE 2: Synchronous targeted connection. Identifies the device model
    // by reading GATT FF90/FF96, resolves the protocol (Legacy vs Modbus),
    // binds the TX characteristic (FFE5/FFE9). Blocks up to timeout_ms.
    //   On success:  m_protocolMode is valid, tx_char bound, returns true.
    //   On failure:  tx_char cleared, m_protocolMode = Unknown, returns false.
    // -----------------------------------------------------------------
    bool connectToDevice(uint64_t address, int timeout_ms = 8000);

    // -----------------------------------------------------------------
    // PHASE 2b: Synchronous targeted connection for Fredorch F21S/T8.
    //   Binds the FFB0 -> FFB1 TX characteristic (falling back to the
    //   legacy FFE5 -> FFE9 layout if FFB1 is absent), best-effort
    //   subscribes to the FFB2 notify characteristic, and sets
    //   m_protocolMode to FredorchF21S. Blocks up to timeout_ms.
    //   On success:  tx_char bound, m_protocolMode = FredorchF21S, returns true.
    //   On failure:  tx_char cleared, m_protocolMode = Unknown, returns false.
    // -----------------------------------------------------------------
    bool connectToFredorch(uint64_t address, int timeout_ms = 8000);

    // -----------------------------------------------------------------
    // PHASE 3: Low-latency fire-and-forget speed command (LATEST-WINS).
    //   - If a previous WriteValueAsync is still pending, it is CANCELLED
    //     and the new write is dispatched. This guarantees that if speeds
    //     arrive faster than the BLE link can drain (e.g. 50ms cadence),
    //     the device is always commanded at the freshest value.
    //   - speed: 0..255 (0 = stop).
    //   - Returns true if the write was dispatched to WinRT (NOT meaning
    //     it was ACK'd — WriteWithoutResponse has no ACK).
    //   - Thread-safe; safe to call concurrently from the tracking thread.
    // -----------------------------------------------------------------
    bool sendSpeedCommand(uint8_t speed);

    // -----------------------------------------------------------------
    // PHASE 3b: Send speed + wait for WinRT Completed delegate to fire.
    //   - status_out receives the GattCommunicationStatus of the op:
    //       Success (0)    : packet delivered to device GATT server
    //       InvalidPdu (2) : device rejected the packet (firmware state?)
    //       others         : transient (DeviceNotReady, RadioFailure, ...)
    //   - timeout_ms: max wait for the delegate (typically 50-200ms;
    //     covers a full connection interval + device processing).
    //   - Returns true if the delegate fired within timeout (regardless
    //     of Gatt status — caller checks status_out).
    //   - Returns false on internal cancel/exception/timeout.
    //   - Thread-confined: must be called from the thread that owns the
    //     WinRtBleManager's event loop (i.e. the thread that called
    //     connectToDevice).
    // -----------------------------------------------------------------
    bool sendSpeedCommandAndWait(uint8_t speed,
                                 int timeout_ms,
                                 int* status_out = nullptr); // int cast of GattCommunicationStatus

    // -----------------------------------------------------------------
    // PHASE 3c: Send an arbitrary raw byte frame (fire-and-forget,
    //   LATEST-WINS coalescing). Used by FredorchF21sController to emit
    //   Modbus RTU frames over the BLE link. Returns true if the write
    //   was dispatched to WinRT (NOT meaning it was ACK'd —
    //   WriteWithoutResponse has no ACK).
    //   - Thread-safe; safe to call concurrently from the tracking thread.
    // -----------------------------------------------------------------
    bool sendRawFrame(const std::vector<uint8_t>& frame);

    // -----------------------------------------------------------------
    // PHASE 3e: Queued raw-frame write (NO LATEST-WINS cancel).
    //   Fire-and-forget: dispatches WriteValueWithoutResponse and stores
    //   the IAsyncOperation in a 32-slot ring buffer so it is not destroyed
    //   (GC'd) before the driver completes it — the depth is sized so old
    //   ops survive a 200-300 ms GattStatus stall on the YXLinksSPP bridge
    //   without being wrapped-over and released. Multiple queued writes
    //   are bundled by the Windows BLE driver into a single connection
    //   event, reducing 3-frame sequences (Play OFF + Step + Play ON)
    //   from ~150 ms to ~20-30 ms.
    //   Thread-safe; safe to call from the worker thread.
    //   Returns true if the write was dispatched to WinRT.
    // -----------------------------------------------------------------
    bool sendRawFrameQueued(const std::vector<uint8_t>& frame);

    // -----------------------------------------------------------------
    // Number of dispatched writes whose GATT Completed delegate has NOT
    // fired yet. Intended for callers that dispatch fire-and-forget
    // writes in rapid succession and want to "settle" (wait for the
    // in-flight write to complete) before the next dispatch, so the
    // LATEST-WINS coalescing cancel does not eat a packet that has not
    // left the radio yet. Thread-safe.
    // -----------------------------------------------------------------
    int inFlightWriteCount() const;

    // -----------------------------------------------------------------
    // PHASE 3d: Send an arbitrary raw byte frame and wait for the GATT
    //   Completed delegate to fire (blocking variant of sendRawFrame).
    //   - status_out receives the GattCommunicationStatus as an int
    //     (0 = Success, 2 = InvalidPdu, ...); -1 if no result.
    //   - Returns true if the delegate fired within timeout_ms AND the
    //     Gatt status was Success.
    //   - Thread-confined: must be called from the thread that owns the
    //     WinRtBleManager's event loop (i.e. the thread that called
    //     connectToDevice / connectToFredorch).
    // -----------------------------------------------------------------
    bool sendRawFrameAndWait(const std::vector<uint8_t>& frame,
                             int timeout_ms,
                             int* status_out = nullptr);

    // -----------------------------------------------------------------
    // TEARDOWN. Safe to call on an unconnected manager (no-op).
    //   - **BLOCKING**: does not return until (a) any in-flight
    //     WriteValueAsync has either landed its Completed delegate or was
    //     Cancel'd, and (b) the COM handle is released.
    //   - Internally PUMPS the Windows message loop (QCoreApplication::
    //     processEvents) so that WinRT Completed delegates fire on this
    //     thread — a plain `std::this_thread::yield()` drain will never
    //     complete because WinRT delegates only run when the message pump
    //     is fed (this was the "still in flight after 500ms drain" bug).
    // -----------------------------------------------------------------
    void disconnectDevice();

    bool isConnected() const { return m_connected.load(std::memory_order_acquire); }

    DeviceProtocolMode getProtocolMode() const {
        std::lock_guard l(m_mtx);
        return m_protocolMode;
    }

    // -----------------------------------------------------------------
    // Diagnostic detail from the last successful/unsuccessful handshake.
    // Intended to be shown in GUI error dialogs. Thread-safe.
    // -----------------------------------------------------------------
    QString lastHandshakeError() const {
        std::lock_guard l(m_mtx);
        return m_lastError;
    }

    // Convenience: set a diagnostic message (used by internal failure paths).
    void setLastError(const QString& s) {
        std::lock_guard l(m_mtx);
        m_lastError = s;
    }

    // -----------------------------------------------------------------
    // Optional raw-notification callback (Fredorch FFB2 / 0xFFB2 notify).
    // Every ValueChanged payload on the Fredorch Rx characteristic is
    // forwarded to this callback IN ADDITION to the log line. The callback
    // runs on the WinRT callback thread (UI/STA thread) and MUST be
    // non-blocking and exception-safe. Pass nullptr to clear.
    // Intended use: FredorchF21sController captures Modbus RTU response
    // frames (e.g. readPlayState() waiting for the 0x0069 read reply).
    // -----------------------------------------------------------------
    using RawNotifyCallback =
        std::function<void(const uint8_t* data, size_t length)>;
    void setRawNotifyCallback(RawNotifyCallback cb);

signals:
    // Convenience signals (emitted from the sync API — safe to connect for
    // logging / status UIs):
    void deviceConnected();
    void errorOccurred(QString error);

private:
    std::vector<uint8_t> generateModbusPacketForSpeed(uint8_t speed);
    uint16_t calculateCRC16(const std::vector<uint8_t>& data);

    // Core raw-frame dispatch (LATEST-WINS coalescing + fire-and-forget).
    // Shared by sendSpeedCommand() and sendRawFrame(). `contextTag` is a
    // short label used in the Completed-delegate log line.
    bool dispatchRawFrame(const std::vector<uint8_t>& payload,
                          const char* contextTag);

    // Core raw-frame dispatch + wait (blocking variant). Shared by
    // sendSpeedCommandAndWait() and sendRawFrameAndWait().
    bool dispatchRawFrameAndWait(const std::vector<uint8_t>& payload,
                                 int timeout_ms,
                                 int* status_out);

    std::unique_ptr<Impl> pimpl;
};
