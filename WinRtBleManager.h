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

enum class DeviceProtocolMode {
    Unknown,
    LegacyHismith,
    HismithMini
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

signals:
    // Convenience signals (emitted from the sync API — safe to connect for
    // logging / status UIs):
    void deviceConnected();
    void errorOccurred(QString error);

private:
    std::vector<uint8_t> generateModbusPacketForSpeed(uint8_t speed);
    uint16_t calculateCRC16(const std::vector<uint8_t>& data);

    std::unique_ptr<Impl> pimpl;
};
