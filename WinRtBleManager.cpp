// WinRtBleManager.cpp
//
// WinRT Bluetooth LE client for Hismith / Wildolo / Fredorch hardware.
// - Synchronous connect/disconnect API for easy integration with the
//   existing sequential main loop (test_hismith / run_funscript / etc.).
// - Low-latency Writes (WriteWithoutResponse) for speed command dispatch.
// - Thread-safe via mutex + atomics; safe to call concurrently from the
//   tracking loop and the UI thread.
//
// Protocol notes (from the project spec):
//   LegacyHismith : 4-byte 0xAA packet { 0xAA, 0x04, speed, speed^0x04 }
//   HismithMini   : Modbus RTU frame (0x01, 0x10, addr 0x006B, CRC16)
//
// GATT layout (16-bit custom UUIDs, base 0000ffxx-0000-1000-8000-00805f9b34fb):
//   SERVICE_INFO_UUID (0xff90) -> CHAR_RX_MODEL (0xff96) -> model id
//   SERVICE_TX_UUID   (0xffe5) -> CHAR_TX_DATA  (0xffe9) -> PWM stream
//
// All code, logs, comments are in ENGLISH per project spec (.roo/rules/agents.md).

#include "WinRtBleManager.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <future>
#include <vector>

#include <QDebug>
#include <QString>
#include <QCoreApplication>
#include <QEventLoop>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Devices.Bluetooth.Advertisement.h>
#include <winrt/Windows.Devices.Bluetooth.GenericAttributeProfile.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Security.Cryptography.h>

using namespace winrt::Windows::Devices::Bluetooth;
using namespace winrt::Windows::Devices::Bluetooth::Advertisement;
using namespace winrt::Windows::Devices::Bluetooth::GenericAttributeProfile;
using namespace winrt::Windows::Storage::Streams;

namespace
{
    // -------------------------------------------------------------------------
    // GATT UUID constants (16-bit custom UUIDs per vendor spec).
    // Full 128-bit form: 0000xxxx-0000-1000-8000-00805f9b34fb.
    // -------------------------------------------------------------------------
    static constexpr winrt::guid SERVICE_INFO_UUID{
        0xFF90, 0x0000, 0x1000,
        {0x80, 0x00, 0x00, 0x80, 0x5F, 0x9B, 0x34, 0xFB}};
    static constexpr winrt::guid CHAR_RX_MODEL_UUID{
        0xFF96, 0x0000, 0x1000,
        {0x80, 0x00, 0x00, 0x80, 0x5F, 0x9B, 0x34, 0xFB}};
    static constexpr winrt::guid SERVICE_TX_UUID{
        0xFFE5, 0x0000, 0x1000,
        {0x80, 0x00, 0x00, 0x80, 0x5F, 0x9B, 0x34, 0xFB}};
    static constexpr winrt::guid CHAR_TX_DATA_UUID{
        0xFFE9, 0x0000, 0x1000,
        {0x80, 0x00, 0x00, 0x80, 0x5F, 0x9B, 0x34, 0xFB}};

    // Model identifiers (lower-case hex strings) seen in the wild on the GATT
    // FF96 RX characteristic of Legacy Hismith hardware. Anything NOT in this
    // list is treated as the "Hismith-Mini / Fredorch" Modbus variant.
    const std::vector<std::string>& legacy_models()
    {
        static const std::vector<std::string> v = {
            "1001", "1002", "1003", "2001", "3001", "1006"
        };
        return v;
    }

    // Advertisement-name keywords (lower-case) we recognize.
    const char* const NAME_KEYWORDS[] = { "hismith", "wildolo", "fredorch" };

    inline std::string to_lower(std::string s)
    {
        for (auto& c : s)
        {
            if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        }
        return s;
    }

    inline bool name_matches_hismith(std::wstring_view name)
    {
        if (name.empty()) return false;
        std::string utf8;
        utf8.reserve(name.size());
        // Simple UTF-16 -> UTF-8 for the ASCII range we care about.
        for (size_t i = 0; i < name.size(); ++i)
        {
            unsigned short c = name[i];
            if (c < 0x80)
            {
                utf8.push_back((char)c);
            }
            // (non-ASCII ignored; keywords are ASCII)
        }
        utf8 = to_lower(utf8);
        for (const char* kw : NAME_KEYWORDS)
        {
            if (utf8.find(kw) != std::string::npos)
            {
                return true;
            }
        }
        return false;
    }
} // anonymous namespace

// ---------------------------------------------------------------------------
// Impl
// ---------------------------------------------------------------------------
struct WinRtBleManager::Impl
{
    // Live BLE device handle (nullptr when not connected). Held only to keep
    // the COM object alive while coroutines run; all reads/writes go through
    // characteristics. Mutated only inside connectHandshakeImpl/disconnect.
    BluetoothLEDevice device { nullptr };
    GattCharacteristic txCharacteristic { nullptr };

    // Counts how many WriteValueAsync operations are currently in flight.
    // disconnectDevice waits for this to drain before dropping `device`.
    std::atomic<int> inFlightWrites { 0 };

    // -------------------------------------------------------------------------
    // Latest outstanding WriteValueAsync handle — LATEST-WINS coalescing:
    //   (a) sendSpeedCommand: if a previous write is still pending, CANCEL
    //       it before dispatching the new one, so the device is always
    //       commanded at the freshest value.  Required for 50ms speed
    //       cadence where the BLE link (7.5-15ms connection-interval)
    //       could queue two writes out of order otherwise.
    //   (b) disconnectDevice: instead of just spin-waiting on
    //       inFlightWrites (which would never reach zero if the delegate
    //       hasn't fired on this thread — see log line "2 writes still
    //       in flight after 500ms drain — abandoning"), we CANCEL the
    //       pending op directly AND pump the message queue so any
    //       remaining Completed delegates can fire.
    //
    // Stored as the concrete IAsyncOperation<T> base (cppwinrt does not expose a type-erased async handle) so we do not need
    // to name T (GattCommunicationStatus) in this struct.
    // -------------------------------------------------------------------------
    winrt::Windows::Foundation::IAsyncOperation<
        winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::GattCommunicationStatus> pendingWrite;

    // Diagnostic state set by disconnectDevice()'s drain loop so the final
    // log line can report precise drain outcome instead of the opaque
    // "still in flight after 500ms drain — abandoning".
    int            pendingCancelledDuringDrain { 0 };
    long long      drainDurationMs             { 0 };

    // Handle for the ConnectionParametersChanged event subscription.
    // Subscribed in the handshake, released in disconnectDevice.
    // It is stored in Impl (pimpl) so the event stays attached to the device
    // for the full lifetime of the BLE link (dropping it would tear down the
    // COM handler and we would lose the negotiated-parameter log).
    winrt::event_token connectParamsToken;
};

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------
WinRtBleManager::WinRtBleManager(QObject* parent)
    : QObject(parent), pimpl(std::make_unique<Impl>())
{
    // NOTE: We intentionally do NOT call winrt::init_apartment() here.
    // Qt6 initializes COM as STA on the main UI thread (clipboard, OLE,
    // drag-and-drop). Calling init_apartment(MTA) on an STA thread causes
    // a hresult_error crash. Instead, C++/WinRT's coroutine machinery
    // automatically resumes `co_await` continuations on the Windows thread
    // pool (MTA threads), so GATT reads/writes never block the UI thread
    // even though the caller (our sync-wait via std::promise) sits on the
    // STA thread.
    // See: https://forum.qt.io/topic/136235/using-winrt-cpp-with-qt-6-x-x
    // https://learn.microsoft.com/en-us/windows/apps/develop/cpp-winrt/concurrency-2
}

WinRtBleManager::~WinRtBleManager()
{
    // Force-close any open GATT link before the Impl is destroyed so that
    // in-flight writes don't dereference a torn-down lambda capture.
    disconnectDevice();
    // Note: WinRT is still initialized for the entire process (Qt app). We
    // do NOT call uninit_apartment here because the process lifetime may
    // outlive this manager instance and the WinRT runtime is per-process.
}

// ---------------------------------------------------------------------------
// Phase 1: findHismithDevices — scan WITHOUT holding any device
// ---------------------------------------------------------------------------
std::vector<BleDeviceInfo>
WinRtBleManager::findHismithDevices(int timeout_ms)
{
    if (timeout_ms <= 0)
    {
        return {};
    }

    struct Entry { QString name; uint64_t address = 0; };
    std::vector<Entry> entries;
    std::mutex localMtx;

    auto watcher = BluetoothLEAdvertisementWatcher();
    watcher.ScanningMode(BluetoothLEScanningMode::Active);

    watcher.Received([this, &entries, &localMtx](
        const BluetoothLEAdvertisementWatcher&,
        const BluetoothLEAdvertisementReceivedEventArgs& args)
    {
        const uint64_t address = args.BluetoothAddress();
        if (address == 0) return;

        const auto adv = args.Advertisement();
        const auto localName = adv.LocalName();
        if (localName.empty()) return;

        std::wstring nameStr(localName.data(), localName.size());
        if (!name_matches_hismith(nameStr))
        {
            return;
        }

        const QString qName = QString::fromWCharArray(nameStr.c_str());

        std::lock_guard l(localMtx);
        for (const auto& e : entries)
        {
            if (e.address == address)
            {
                return; // dedup by MAC — first sighting wins
            }
        }
        entries.push_back({ qName, address });
    });

    watcher.Start();

    // Pump Qt events while we block so that Qt-network code paths (e.g. any
    // pending HereSphereSync / VLC polling in the same UI thread) progress.
    // findHismithDevices sits on the UI thread during refreshDevices().
    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline)
    {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    }

    watcher.Stop();

    std::vector<BleDeviceInfo> out;
    out.reserve(entries.size());
    std::lock_guard l(localMtx);
    for (const auto& e : entries)
    {
        out.push_back({ e.name, e.address });
    }

    if (out.empty())
    {
        qDebug() << "findHismithDevices: no Hismith/Wildolo/Fredorch "
                 << "advertisement seen in" << timeout_ms << "ms";
    }
    return out;
}

// ---------------------------------------------------------------------------
// Core coroutine — handshake.
//
// Signature notes:
// - `self` is captured by RAW pointer. This is safe because WinRtBleManager
//   is the app-lifetime singleton owned by MainWindow, and the only code path
//   that destroys it also drains in-flight operations first.
// - `out_promise` is set_value'd exactly once, whether success or failure.
// ---------------------------------------------------------------------------
// out_promise is a shared_ptr so that the coroutine's `complete()` lambda
// can keep it alive past the caller's scope. Without this, if the caller
// hits the timeout path and `return`s, the stack-local std::promise<>
// is destroyed; a later coroutine resume calling set_value() on the
// dead promise would dereference a freed std::mutex (Access Violation).
using HandshakePromise = std::shared_ptr<std::promise<bool>>;

static winrt::Windows::Foundation::IAsyncAction
connectHandshakeImpl(
    WinRtBleManager* self,
    uint64_t address,
    WinRtBleManager::Impl* impl,
    HandshakePromise out_promise)
{
    // Single-shot completion latch.
    std::atomic<bool> finished{ false };
    auto complete = [&](bool ok) {
        if (finished.exchange(true))
        {
            return;
        }
        try
        {
            out_promise->set_value(ok);
        }
        catch (const std::future_error&)
        {
            // promise already fulfilled — ignore
        }
    };

    // reason parameter: diagnostic detail recorded into m_lastError for the
    // GUI (only on failure paths; ignored on success).
    auto cleanup_and_complete = [&](bool ok, const QString& reason = QString()) {
        // Clear the bound GATT objects under the manager mutex so that a
        // concurrent sendSpeedCommand() sees a consistent state and
        // disconnectDevice() cannot race with this coroutine's cleanup.
        {
            std::lock_guard l(self->m_mtx);
            impl->device = nullptr;
            impl->txCharacteristic = nullptr;
        }
        if (!ok && !reason.isEmpty())
        {
            self->setLastError(reason);
        }
        complete(ok);
    };

    try
    {
        // 1. Open device by address.
        BluetoothLEDevice device = co_await BluetoothLEDevice::FromBluetoothAddressAsync(address);
        if (!device)
        {
            qDebug().noquote() << "Handshake: BluetoothLEDevice null (address 0x"
                               << QString::number(address, 16) << ")";
            cleanup_and_complete(false,
                QStringLiteral("Step 1/6 FAILED: BluetoothLEDevice::FromBluetoothAddressAsync returned null device handle. "
                               "The device dropped its BLE radio, went out of range, or was power-cycled during the handshake."));
            co_return;
        }
        impl->device = device;
        qDebug().noquote() << "Handshake: device link OK";

        // 1b. REQUEST LOW-LATENCY CONNECTION PARAMETERS.
        //
        // API (Win11 SDK 22000, IBluetoothLEDevice6):
        //   BluetoothLEDevice::RequestPreferredConnectionParameters(params)
        //   -> BluetoothLEPreferredConnectionParametersRequest
        // The request object exposes a Status():
        //   Success / DeviceNotAvailable / AccessDenied
        // Windows queues LL_Exchange_Connection_Parameters_Request on the
        // link with the values we asked for and the device may accept,
        // reject, or negotiate to a wider window on its own.
        //
        // Default Windows intervals are 30-50ms (widening to 100-200ms on
        // idle) — that is the primary source of the 13-100ms jitter observed
        // in sendSpeedCommand dispatches. ThroughputOptimized() is the
        // low-latency profile exposed in SDK 22000:
        //   ConnectionInterval : 7.5 - 15 ms (min 6 / max 12 in 1.25ms units)
        //   SlaveLatency       : 0 (no "skip N events" mode)
        try
        {
            const auto request = device.RequestPreferredConnectionParameters(
                BluetoothLEPreferredConnectionParameters::ThroughputOptimized());
            const auto status = request.Status();
            switch (status)
            {
            case BluetoothLEPreferredConnectionParametersRequestStatus::Success:
                qDebug().noquote()
                    << "Handshake: ThroughputOptimized — Success (queued)";
                break;
            case BluetoothLEPreferredConnectionParametersRequestStatus::DeviceNotAvailable:
                qWarning().noquote()
                    << "Handshake: ThroughputOptimized — DeviceNotAvailable; "
                    << "link may not be up yet — request will still be queued.";
                break;
            case BluetoothLEPreferredConnectionParametersRequestStatus::AccessDenied:
                qWarning().noquote()
                    << "Handshake: ThroughputOptimized — AccessDenied; "
                    << "Bluetooth may be policy-locked by Group Policy / enterprise settings.";
                break;
            default:
                qDebug().noquote()
                    << "Handshake: ThroughputOptimized — Unspecified "
                    << "(queued; the ConnectionParametersChanged event will "
                    << "log the final negotiated values)";
                break;
            }
        }
        catch (const winrt::hresult_error& ex)
        {
            // Non-fatal — the link still works, just with a wider default interval.
            qWarning().noquote()
                << "Handshake: RequestPreferredConnectionParameters threw:"
                << QString::fromWCharArray(ex.message().c_str());
        }

        // 1c. SUBSCRIBE to ConnectionParametersChanged to LOG that a
        // parameter change actually happened (and that our request was
        // processed rather than silently dropped). SDK 22000's
        // signature for this event is
        //   (BluetoothLEDevice, IInspectable)
        // so we cannot read the new parameters from the event args directly.
        // Firing the logger proves the exchange happened; the user can
        // verify the final interval in Windows Bluetooth settings if needed,
        // but the empirical latency measurements tell the truth.
        //
        // The event_token lives in Impl (pimpl->connectParamsToken). We do
        // NOT explicitly revoke it: dropping `pimpl->device` in
        // disconnectDevice() destroys the COM object and auto-detaches
        // its handlers. The token slot just holds the
        // remove_ConnectionParametersChanged binding alive.
        //
        // Callback thread: WinRT worker pool (not the Qt UI thread). The
        // handler only emits a log line — no shared mutable state.
impl->connectParamsToken = device.ConnectionParametersChanged(
    [](const BluetoothLEDevice& dev,
       winrt::Windows::Foundation::IInspectable const&)
    {
        try
        {
            const auto devName = dev.Name();
            qDebug().noquote()
                << "Handshake: ConnectionParametersChanged fired - the link "
                << "re-negotiated its Connection Interval. (Device: "
                << (devName.empty() ? winrt::hstring(L"<unnamed>") : devName)
                << "). If the measured write latency dropped to ~7-15ms,"
                << " the request was accepted; if it stayed in the 30-100ms"
                << " range, the firmware kept its wider profile.";
        }
        catch (const winrt::hresult_error& ex)
        {
            qWarning().noquote()
                << "ConnectionParametersChanged: WinRT exception:"
                << QString::fromWCharArray(ex.message().c_str());
        }
        catch (const std::exception& ex)
        {
            qWarning().noquote()
                << "ConnectionParametersChanged: C++ exception:"
                << ex.what();
        }
        catch (...)
        {
            qWarning() << "ConnectionParametersChanged: unknown exception";
        }
    });

        // 2. Resolve the INFO service (FF90).
        const auto infoSvcResult = co_await device.GetGattServicesForUuidAsync(SERVICE_INFO_UUID);
        if (infoSvcResult.Status() != GattCommunicationStatus::Success
            || infoSvcResult.Services().Size() == 0)
        {
            qDebug().noquote() << "Handshake: INFO service (FF90) NOT FOUND (status:"
                               << (int)infoSvcResult.Status() << ")";
            cleanup_and_complete(false,
                QStringLiteral("Step 2/6 FAILED: GATT service 0xFF90 (INFO) not found on the device (Gatt status=%1). "
                               "The device firmware may not expose the expected GATT layout, "
                               "or another client (Intiface?) still holds the connection.")
                .arg((int)infoSvcResult.Status()));
            co_return;
        }
        const auto infoSvc = infoSvcResult.Services().GetAt(0);

        // 3. Resolve and read the model char (FF96).
        const auto rxCharResult = co_await infoSvc.GetCharacteristicsForUuidAsync(CHAR_RX_MODEL_UUID);
        if (rxCharResult.Status() != GattCommunicationStatus::Success
            || rxCharResult.Characteristics().Size() == 0)
        {
            qDebug().noquote() << "Handshake: MODEL RX char (FF96) NOT FOUND (status:"
                               << (int)rxCharResult.Status() << ")";
            cleanup_and_complete(false,
                QStringLiteral("Step 3/6 FAILED: GATT characteristic 0xFF96 (MODEL id) not found under service 0xFF90 (Gatt status=%1).")
                .arg((int)rxCharResult.Status()));
            co_return;
        }
        const auto rxChar = rxCharResult.Characteristics().GetAt(0);

        const auto readResult = co_await rxChar.ReadValueAsync(BluetoothCacheMode::Uncached);
        if (readResult.Status() != GattCommunicationStatus::Success)
        {
            qDebug().noquote() << "Handshake: MODEL read FAILED (status:"
                               << (int)readResult.Status() << ")";
            cleanup_and_complete(false,
                QStringLiteral("Step 4/6 FAILED: ReadValueAsync(0xFF96) returned Gatt status=%1 — the device rejected the model-id read. "
                               "Common cause: another GATT client is still locked in (e.g. Intiface Central). "
                               "Close it or wait 5-10 s and retry.")
                .arg((int)readResult.Status()));
            co_return;
        }

        // 4. Identify protocol mode.
        auto buffer = readResult.Value();
        auto reader = DataReader::FromBuffer(buffer);
        std::string model;
        while (reader.UnconsumedBufferLength() > 0)
        {
            const unsigned char b = reader.ReadByte();
            char hexstr[3] = { 0, 0, 0 };
            std::snprintf(hexstr, 3, "%02x", b);
            model.append(hexstr);
        }
        qDebug().noquote() << "Handshake: model id (hex):" << QString::fromStdString(model);

        DeviceProtocolMode mode = DeviceProtocolMode::HismithMini; // default
        for (const auto& m : legacy_models())
        {
            if (m == model)
            {
                mode = DeviceProtocolMode::LegacyHismith;
                break;
            }
        }
        {
            std::lock_guard l(self->m_mtx);
            self->m_protocolMode = mode;
        }
        qDebug().noquote() << "Handshake: protocol:"
                           << (mode == DeviceProtocolMode::LegacyHismith
                               ? "LegacyHismith (0xAA stream)"
                               : "HismithMini (Modbus 0x6B)");

        // 5. Resolve the TX service (FFE5) and data char (FFE9).
        const auto txSvcResult = co_await device.GetGattServicesForUuidAsync(SERVICE_TX_UUID);
        if (txSvcResult.Status() != GattCommunicationStatus::Success
            || txSvcResult.Services().Size() == 0)
        {
            qDebug().noquote() << "Handshake: TX service (FFE5) NOT FOUND (status:"
                               << (int)txSvcResult.Status() << ")";
            cleanup_and_complete(false,
                QStringLiteral("Step 5/6 FAILED: GATT service 0xFFE5 (TX) not found on the device (Gatt status=%1).")
                .arg((int)txSvcResult.Status()));
            co_return;
        }
        const auto txSvc = txSvcResult.Services().GetAt(0);

        const auto txCharResult = co_await txSvc.GetCharacteristicsForUuidAsync(CHAR_TX_DATA_UUID);
        if (txCharResult.Status() != GattCommunicationStatus::Success
            || txCharResult.Characteristics().Size() == 0)
        {
            qDebug().noquote() << "Handshake: TX data char (FFE9) NOT FOUND (status:"
                               << (int)txCharResult.Status() << ")";
            cleanup_and_complete(false,
                QStringLiteral("Step 6/6 FAILED: GATT characteristic 0xFFE9 (TX data) not found under service 0xFFE5 (Gatt status=%1).")
                .arg((int)txCharResult.Status()));
            co_return;
        }

        {
            std::lock_guard l(self->m_mtx);
            impl->device = device;
            impl->txCharacteristic = txCharResult.Characteristics().GetAt(0);
        }
        self->setLastError(QString()); // clear any prior diagnostic
        complete(true);
        qDebug().noquote() << "Handshake: SUCCESS — TX pipeline ready";
        co_return;
    }
    catch (const winrt::hresult_error& ex)
    {
        const wchar_t* msg = ex.message().c_str();
        qWarning().noquote() << "Handshake: WinRT exception:"
                             << (msg && *msg ? QString::fromWCharArray(msg).toStdString()
                                             : std::string("unknown"));
        cleanup_and_complete(false);
    }
    catch (const std::exception& ex)
    {
        qWarning().noquote() << "Handshake: C++ exception:" << ex.what();
        cleanup_and_complete(false);
    }
    catch (...)
    {
        qWarning().noquote() << "Handshake: unknown exception";
        cleanup_and_complete(false);
    }
}

// ---------------------------------------------------------------------------
// Phase 2: connectToDevice (synchronous)
// ---------------------------------------------------------------------------
bool WinRtBleManager::connectToDevice(uint64_t address, int timeout_ms)
{
    if (!address)
    {
        qWarning() << "connectToDevice: address == 0 — aborted";
        return false;
    }

    // Idempotency: cleanly close any previous link first.
    if (m_connected.load(std::memory_order_acquire))
    {
        disconnectDevice();
    }

    // Zero out stale impl state before the handshake starts.
    {
        std::lock_guard l(m_mtx);
        pimpl->device = nullptr;
        pimpl->txCharacteristic = nullptr;
        pimpl->pendingWrite = nullptr;  // clear any stale pending write handle
        m_protocolMode = DeviceProtocolMode::Unknown;
    }

    // Heap-allocated via shared_ptr so that the coroutine's `complete()`
    // lambda can extend its lifetime past our `return`. A stack-local
    // promise would be destroyed on the timeout path, then a later
    // coroutine-resume calling set_value() would crash (use-after-free
    // of the promise's internal mutex).
    auto promise = std::make_shared<std::promise<bool>>();
    auto fut = promise->get_future();

    // Launch the coroutine. CRITICAL: the returned IAsyncAction MUST be
    // held in a live variable across the wait_for() call below.
    //
    // WinRT semantics: destroying the IAsyncAction handle (drop ref to 0)
    // marks the op as "detached" — the runtime may drop its queued work on
    // a later idle pass, so the continuation (co_await steps) does NOT run
    // to completion. The promise is then never fulfilled and we see a
    // spurious TIMEOUT even though the BLE link is fine.
    //
    // Keeping `asyncOp` alive in our scope until the future state is settled
    // (or we time out) gives the WinRT runtime a stable handle for the full
    // GATT handshake chain. It is then destroyed at scope exit or cancelled
    // explicitly on timeout.
    auto asyncOp = connectHandshakeImpl(this, address, pimpl.get(), promise);

    // PUMP Qt events while waiting.
    //
    // WinRT coroutines on an STA thread (Qt's UI thread) deliver their
    // continuations via the Windows message queue (PostMessage). A plain
    // fut.wait_for(N) would block the thread and prevent message delivery —
    // resulting in a spurious TIMEOUT. By calling QCoreApplication::
    // processEvents() every 10ms, we pump the message queue so the WinRT
    // runtime can resume the coroutine's co_await steps.
    //
    // This is the standard pattern for "async->sync" bridging in Qt+WinRT
    // apps. See: https://forum.qt.io/topic/136235
    std::future_status wait_result = std::future_status::timeout;
    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline)
    {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        wait_result = fut.wait_for(std::chrono::milliseconds(10));
        if (wait_result == std::future_status::ready)
        {
            break;
        }
    }
    bool ok = false;

    if (wait_result == std::future_status::ready)
    {
        ok = fut.get();
        if (ok)
        {
            m_connected.store(true, std::memory_order_release);
            emit deviceConnected();
            qDebug() << "connectToDevice: SUCCESS";

            // -------------------------------------------------------------
            // PRIMING WRITE (speed=0): some Hismith firmwares reject the first
            // real speed command if no write has been seen since the
            // connection parameters settled. Sending a known-good "stop"
            // write (speed 0) right after the handshake arms the device's
            // GATT state machine so subsequent speed commands are accepted.
            //
            // Fire-and-forget variant + a short message-pump so the
            // Completed delegate (which LOGs the real AsyncStatus +
            // GattStatus) has time to run. The write result itself is
            // non-fatal; set_hismith_speed() will retry if the device
            // still rejects its own first command.
            // -------------------------------------------------------------
            {
                const bool dispatched = sendSpeedCommand(0);
                // Give the Completed delegate time to run. The delegate
                // will print its OWN log line ("sendSpeedCommand: OK/NOT OK
                // — AsyncStatus ... GattStatus ...") with the real status.
                if (dispatched)
                {
                    const auto primingDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(400);
                    while (std::chrono::steady_clock::now() < primingDeadline
                           && pimpl->inFlightWrites.load(std::memory_order_acquire) > 0)
                    {
                        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
                    }
                }
                else
                {
                    qWarning().noquote()
                        << "connectToDevice: priming write (speed=0) NOT DISPATCHED";
                }
            }
        }
        else
        {
            m_connected.store(false, std::memory_order_release);
            emit errorOccurred(QString("BLE handshake failed for address 0x%1")
                               .arg(address, 0, 16));
            qWarning() << "connectToDevice: handshake FAILED (see logs above)";
        }
    }
    else
    {
        // Timeout: the handshake coroutine may still be running and will
        // eventually fulfill the promise. We abandon the wait but must make
        // sure we don't leave a dangling txCharacteristic. Safest is to
        // disconnect and wait briefly for in-flight state to settle.
        m_connected.store(false, std::memory_order_release);
        emit errorOccurred(QString("BLE connect TIMEOUT after %1 ms")
                           .arg(timeout_ms));
        qWarning() << "connectToDevice: TIMEOUT after" << timeout_ms << "ms — tearing down";

        // Give callers a more actionable error string than a bare "TIMEOUT".
        // If a previous step already recorded a more specific reason, preserve
        // it; otherwise fall back to the generic timeout text.
        {
            std::lock_guard l(m_mtx);
            if (m_lastError.isEmpty())
            {
                m_lastError = QStringLiteral("Handshake TIMEOUT after %1 ms — "
                                             "the GATT operation chain exceeded the budget. "
                                             "The device is powered on but slow to respond, "
                                             "or another GATT client (Intiface Central?) is sharing the BLE link.")
                    .arg(timeout_ms);
            }
        }

        // Explicitly cancel the GATT chain now that we are past deadline.
        // Without this, the async op may keep issuing writes/reads that race
        // with disconnectDevice() below.
        try { asyncOp.Cancel(); }
        catch (const winrt::hresult_error&) { /* Already finished — ignore */ }

        // Give the still-running coroutine a small grace period to finish and
        // publish its own cleanup (it clears impl->txCharacteristic), then
        // disconnect() to force any residual state down and drop the COM refs.
        // Pump Qt events during the grace so the coroutine's continuations
        // (which need the message pump to fire on this STA thread) actually
        // run; a plain sleep_for would hang.
        const auto grace_deadline = std::chrono::steady_clock::now()
            + std::chrono::milliseconds(500);
        while (std::chrono::steady_clock::now() < grace_deadline)
        {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        }
        disconnectDevice();
    }

    return ok;
}

// ---------------------------------------------------------------------------
// Phase 3: sendSpeedCommand (low-latency fire-and-forget, LATEST-WINS)
// ---------------------------------------------------------------------------
bool WinRtBleManager::sendSpeedCommand(uint8_t speed)
{
    if (!m_connected.load(std::memory_order_acquire))
    {
        // Not an error per se — but useful to log at DEBUG level only to
        // avoid spamming the log if the caller is polling.
        return false;
    }

    // Snapshot TX characteristic and current protocol mode under the lock.
    GattCharacteristic tx{ nullptr };
    DeviceProtocolMode mode = DeviceProtocolMode::Unknown;
    {
        std::lock_guard l(m_mtx);
        tx = pimpl->txCharacteristic;
        mode = pimpl->pendingWrite ? mode : mode;  // (no-op; kept for clarity)
        mode = m_protocolMode;
    }

    if (!tx)
    {
        qWarning() << "sendSpeedCommand: TX characteristic not bound — packet dropped";
        return false;
    }

    // Build the packet for the active protocol.
    std::vector<unsigned char> payload;
    if (mode == DeviceProtocolMode::LegacyHismith)
    {
        const unsigned char idx = 0x04;
        const unsigned char checksum = (unsigned char)(speed + idx);
        payload = { 0xAA, idx, speed, checksum };
    }
    else if (mode == DeviceProtocolMode::HismithMini)
    {
        payload = generateModbusPacketForSpeed(speed);
    }
    else
    {
        qWarning() << "sendSpeedCommand: protocol mode unresolved — packet dropped";
        return false;
    }

    DataWriter writer;
    for (const auto b : payload)
    {
        writer.WriteByte(b);
    }
    auto buffer = writer.DetachBuffer();

    // -------------------------------------------------------------------------
    // LATEST-WINS coalescing: if a previous WriteValueAsync is still pending
    // (e.g. 50ms cadence, connection-interval ~10ms, so 2 writes can be
    // in flight at the L2 layer), CANCEL it before dispatching the new one.
    // Without this, the BLE stack's internal queue may deliver the OLDER
    // speed AFTER the new one, leaving the motor at the stale value.
    // -------------------------------------------------------------------------
    {
        std::lock_guard l(m_mtx);
        if (pimpl->pendingWrite)
        {
            try { pimpl->pendingWrite.Cancel(); }
            catch (const winrt::hresult_error&) { /* already finished */ }
            pimpl->pendingWrite = nullptr;
        }
        pimpl->inFlightWrites.fetch_add(1, std::memory_order_release);
    }

    try
    {
        auto* selfRaw = this;
        // Hold a strong ref to the async op so we can Cancel it later from
        // sendSpeedCommand (latest-wins) or disconnectDevice (drain).
        auto asyncOp = tx.WriteValueAsync(buffer, GattWriteOption::WriteWithoutResponse);
        {
            std::lock_guard l(m_mtx);
            pimpl->pendingWrite = asyncOp;  // erased to IAsyncOperation
        }
        // The WinRT delegate invokes operator() const. const_cast the pointer
        // so we can touch pimpl's atomic counter from a const context. This is
        // safe: the counter is an atomic and the manager outlives the async op
        // (see disconnectDevice() which drains inFlightWrites before destroying
        // pimpl, and CANCELs any pending write first).
        asyncOp.Completed([selfRaw, speed](winrt::Windows::Foundation::IAsyncOperation<winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::GattCommunicationStatus> const& op,
            winrt::Windows::Foundation::AsyncStatus status)
        {
            auto* selfNC = const_cast<WinRtBleManager*>(selfRaw);
            selfNC->pimpl->inFlightWrites.fetch_sub(1, std::memory_order_release);
            const int intStatus = (int)status;
            // DIAGNOSTICS: log every outcome (ok OR not). This is the
            // fire-and-forget path — there is no caller that observes
            // the result, so the only window into what happened on the
            // BLE link is this log line. Keep it ON even for success.
            int gstat = -1;
            if (intStatus == 0)
            {
                try { gstat = (int)op.GetResults(); }
                catch (const winrt::hresult_error&) { gstat = -1; }
            }
            static const char* kAsyncName[4] = { "Completed", "Canceled", "Error", "Started" };
            const char* an = (intStatus >= 0 && intStatus < 4) ? kAsyncName[intStatus] : "Unknown";
            const bool fullyOk = (intStatus == 0 && gstat == 0); // Completed && Gatt==Success
            if (fullyOk)
            {
                qDebug().noquote()
                    << "sendSpeedCommand: OK          "
                    << "  AsyncStatus: 0 (Completed)"
                    << "  GattStatus:  0 (Success)"
                    << "  (speed=" << (int)speed << ")";
            }
            else
            {
                qWarning().noquote()
                    << "sendSpeedCommand: NOT OK —"
                    << "  AsyncStatus:" << intStatus << "(" << an << ")"
                    << "  GattStatus:"
                    << (intStatus == 0 ? QString::number(gstat) : QString("n/a (op not Completed)"))
                    << "  (speed=" << (int)speed << ")";
            }
        });
        return true;
    }
    catch (const winrt::hresult_error& ex)
    {
        pimpl->inFlightWrites.fetch_sub(1, std::memory_order_release);
        const wchar_t* msg = ex.message().c_str();
        qWarning() << "sendSpeedCommand: WinRT exception:"
                   << (msg && *msg ? QString::fromWCharArray(msg).toStdString()
                                   : std::string("unknown"));
        return false;
    }
    catch (const std::exception& ex)
    {
        pimpl->inFlightWrites.fetch_sub(1, std::memory_order_release);
        qWarning() << "sendSpeedCommand: C++ exception:" << ex.what();
        return false;
    }
}

// ---------------------------------------------------------------------------
// sendSpeedCommandAndWait — blocking variant (pumps events until GATT result)
// ---------------------------------------------------------------------------
bool WinRtBleManager::sendSpeedCommandAndWait(uint8_t speed,
                                               int timeout_ms,
                                               int* status_out)
{
    if (status_out) *status_out = -1;
    if (!m_connected.load(std::memory_order_acquire))
        return false;

    GattCharacteristic tx{ nullptr };
    DeviceProtocolMode mode = DeviceProtocolMode::Unknown;
    {
        std::lock_guard l(m_mtx);
        tx = pimpl->txCharacteristic;
        mode = m_protocolMode;
    }
    if (!tx)
    {
        qWarning() << "sendSpeedCommandAndWait: TX characteristic not bound";
        return false;
    }

    std::vector<unsigned char> payload;
    if (mode == DeviceProtocolMode::LegacyHismith)
    {
        const unsigned char idx = 0x04;
        const unsigned char checksum = (unsigned char)(speed + idx);
        payload = { 0xAA, idx, speed, checksum };
    }
    else if (mode == DeviceProtocolMode::HismithMini)
    {
        payload = generateModbusPacketForSpeed(speed);
    }
    else
    {
        qWarning() << "sendSpeedCommandAndWait: protocol mode unresolved";
        return false;
    }

    DataWriter writer;
    for (const auto b : payload) writer.WriteByte(b);
    auto buffer = writer.DetachBuffer();

    // LATEST-WINS: cancel any pending write
    {
        std::lock_guard l(m_mtx);
        if (pimpl->pendingWrite)
        {
            try { pimpl->pendingWrite.Cancel(); }
            catch (const winrt::hresult_error&) {}
            pimpl->pendingWrite = nullptr;
        }
        pimpl->inFlightWrites.fetch_add(1, std::memory_order_release);
    }

    // Heap-allocated result box — outlives the lambda if the op outlives us.
    //   first  = whether the Completed delegate fired within the timeout
    //   second = GattCommunicationStatus as int; -1 means "no result".
    auto resultBox = std::make_shared<std::pair<bool, int>>(false, -1);
    auto promise = std::make_shared<std::promise<bool>>();
    auto fut = promise->get_future();

    try
    {
        auto* selfRaw = this;
        auto asyncOp = tx.WriteValueAsync(buffer, GattWriteOption::WriteWithoutResponse);
        {
            std::lock_guard l(m_mtx);
            pimpl->pendingWrite = asyncOp;
        }

        asyncOp.Completed([resultBox, promise, selfRaw, speed](
            winrt::Windows::Foundation::IAsyncOperation<
                winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::GattCommunicationStatus> const& op,
            winrt::Windows::Foundation::AsyncStatus status)
        {
            auto* selfNC = const_cast<WinRtBleManager*>(selfRaw);
            selfNC->pimpl->inFlightWrites.fetch_sub(1, std::memory_order_release);

            const int intStatus = ((int)status);
            int gstat = -1;
            bool ok = false;
            if (intStatus == 0)   // AsyncStatus::Completed
            {
                try { gstat = (int)op.GetResults(); }
                catch (const winrt::hresult_error&) { gstat = -1; }
                ok = (gstat == 0);   // Gatt 0 = GattCommunicationStatus::Success
            }
            // --- DIAGNOSTICS (item 1): expose the AsyncStatus so we can tell
            //     "not even Completed" (Canceled/Error/Started) apart from
            //     "Completed but the device rejected the PDU" (Gatt!=Success).
            if (!ok)
            {
                static const char* kAsyncName[4] = { "Completed", "Canceled", "Error", "Started" };
                const char* an = (intStatus >= 0 && intStatus < 4) ? kAsyncName[intStatus] : "Unknown";
                qWarning().noquote() << "sendSpeedCommandAndWait: write NOT OK —"
                                     << "AsyncStatus:" << intStatus << "(" << an << ")"
                                     << "  GattStatus:"
                                     << (intStatus == 0 ? QString::number(gstat) : QString("n/a (op not Completed)"))
                                     << "  (speed=" << (int)speed << ")"
                                     << "  [0=Completed 1=Canceled 2=Error 3=Started; Gatt 0=Success, 2=InvalidPdu]";
            }
            *resultBox = { ok, gstat };
            try { promise->set_value(ok); }
            catch (const std::future_error&) { /* already set */ }
        });

        // Pump until delegate fires or timeout (10ms ticks)
        const auto deadline = std::chrono::steady_clock::now()
                             + std::chrono::milliseconds(timeout_ms);
        bool done = false;
        while (std::chrono::steady_clock::now() < deadline)
        {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
            if (fut.wait_for(std::chrono::milliseconds(10)) == std::future_status::ready)
            {
                done = true;
                break;
            }
        }

        if (done)
        {
            fut.get();
            if (status_out) *status_out = resultBox->second;
            return resultBox->first;   // true only when Completed && Gatt==Success
        }

        // Timeout — cancel and drain briefly
        try { asyncOp.Cancel(); } catch (...) {}
        for (int i = 0; i < 5; ++i)
        {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
            if (fut.wait_for(std::chrono::milliseconds(5)) == std::future_status::ready)
                break;
        }
        return false; // timed out
    }
    catch (const winrt::hresult_error& ex)
    {
        pimpl->inFlightWrites.fetch_sub(1, std::memory_order_release);
        const wchar_t* msg = ex.message().c_str();
        qWarning() << "sendSpeedCommandAndWait: WinRT exception:"
                     << (msg && *msg ? QString::fromWCharArray(msg).toStdString() : std::string("unknown"));
        return false;
    }
    catch (const std::exception& ex)
    {
        pimpl->inFlightWrites.fetch_sub(1, std::memory_order_release);
        qWarning() << "sendSpeedCommandAndWait: C++ exception:" << ex.what();
        return false;
    }
}

// ---------------------------------------------------------------------------
// Teardown (BLOCKING: full drain + explicit Cancel + message pump)
// ---------------------------------------------------------------------------
void WinRtBleManager::disconnectDevice()
{
    const bool wasConnected = m_connected.load(std::memory_order_relaxed);

    // 1. Stop new sends by clearing the TX characteristic first so concurrent
    // sendSpeedCommand() sees it missing and drops the packet.
    {
        std::lock_guard l(m_mtx);
        pimpl->txCharacteristic = nullptr;
        m_protocolMode = DeviceProtocolMode::Unknown;
        pimpl->pendingCancelledDuringDrain = 0;
        pimpl->drainDurationMs = 0;
    }

    if (wasConnected)
    {
        const auto t0 = std::chrono::steady_clock::now();
        const auto deadline = t0 + std::chrono::milliseconds(800);

        // 2. Explicitly CANCEL the most-pending write. Without this, a
        // stalled write whose Completed delegate has not yet fired on this
        // thread would keep inFlightWrites > 0 forever, and the drain loop
        // below would time out (the infamous "still in flight after 500ms
        // drain — abandoning" log line).
        {
            winrt::Windows::Foundation::IAsyncOperation<
            winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::GattCommunicationStatus> pending;
            {
                std::lock_guard l(m_mtx);
                pending = pimpl->pendingWrite;
                pimpl->pendingWrite = nullptr;
            }
            if (pending)
            {
                try { pending.Cancel(); }
                catch (const winrt::hresult_error&) { /* already finished */ }
                ++pimpl->pendingCancelledDuringDrain;
            }
        }

        // 3. Pump the Windows message loop until inFlightWrites drains
        // to zero OR deadline is hit. WinRT Completed delegates fire on
        // this thread via PostMessage — processEvents() delivers them.
        // The previous implementation used std::this_thread::yield()
        // which spins WITHOUT ever dispatching those messages → the
        // drain could never finish within 500ms even though the write
        // had ALREADY completed on the link.
        while (pimpl->inFlightWrites.load(std::memory_order_acquire) > 0
               && std::chrono::steady_clock::now() < deadline)
        {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        }

        const int remaining = pimpl->inFlightWrites.load(std::memory_order_relaxed);
        pimpl->drainDurationMs = std::chrono::duration_cast<std::chrono::milliseconds>
            (std::chrono::steady_clock::now() - t0).count();

        if (remaining > 0)
        {
            qWarning() << "disconnectDevice:" << remaining
                       << "write(s) STILL in flight after"
                       << pimpl->drainDurationMs << "ms drain"
                       << "(canceled" << pimpl->pendingCancelledDuringDrain
                       << "pending) — abandoning";
        }
        else
        {
            qDebug() << "disconnectDevice: drain complete in"
                     << pimpl->drainDurationMs << "ms"
                     << "(canceled" << pimpl->pendingCancelledDuringDrain
                     << "pending)"
                     << (pimpl->drainDurationMs > 500 ? " — SLOW" : "");
        }

        // 4. Clear the ConnectionParametersChanged subscription token. We do
        // NOT need to explicitly revoke it: dropping `pimpl->device` in
        // step 5 destroys the COM object, which auto-detaches all its
        // handlers. Moving the token out just clears the binding slot so a
        // future connect() does not see a stale token.
        {
            winrt::event_token token;
            {
                std::lock_guard l(m_mtx);
                token = std::move(pimpl->connectParamsToken);
            }
            // Scope-exit: `token` is destroyed; the remove_* slot runs and the
            // handler is released. (cppwinrt event_token is move-only; no
            // .revoke() method needed.)
        }

        // 5. Release the BluetoothLEDevice — drops the COM reference and asks
        // Windows to close the physical BLE link, freeing it for Intiface.
        {
            std::lock_guard l(m_mtx);
            pimpl->device = nullptr;
        }

        m_connected.store(false, std::memory_order_release);
        qDebug() << "disconnectDevice: BLE link closed, GATT handles released.";
    }
}

// ---------------------------------------------------------------------------
// Packet builders (private)
// ---------------------------------------------------------------------------
std::vector<uint8_t>
WinRtBleManager::generateModbusPacketForSpeed(uint8_t speed)
{
    // Modbus RTU frame:
    //   01     : unit id (slave)
    //   10     : function "write multiple registers"
    //   00 6B  : start register 0x006B (motor control register block)
    //   00 05  : quantity = 5 registers (10 bytes)
    //   0A    : byte count = 10
    //   [10 payload bytes]
    //   CRC16 (Low, High)
    std::vector<uint8_t> frame;
    frame.reserve(20);
    frame.push_back(0x01);
    frame.push_back(0x10);
    frame.push_back(0x00); frame.push_back(0x6B);
    frame.push_back(0x00); frame.push_back(0x05);
    frame.push_back(0x0A);
    // 5 registers x 2 bytes: speed-out, speed-in, pos-in, pos-out, repeat
    frame.push_back(0x00); frame.push_back(speed);
    frame.push_back(0x00); frame.push_back(speed);
    frame.push_back(0x00); frame.push_back(0x90); // 144 = center
    frame.push_back(0x00); frame.push_back(0x90);
    frame.push_back(0x00); frame.push_back(0x01);

    const auto crc = calculateCRC16(frame);
    frame.push_back(static_cast<uint8_t>(crc & 0xFF));
    frame.push_back(static_cast<uint8_t>((crc >> 8) & 0xFF));
    return frame;
}

uint16_t
WinRtBleManager::calculateCRC16(const std::vector<uint8_t>& data)
{
    // CRC-16/MODBUS polynomial 0xA001 (reflected).
    uint16_t crc = 0xFFFF;
    for (auto b : data)
    {
        crc ^= static_cast<uint16_t>(b);
        for (int bit = 0; bit < 8; ++bit)
        {
            if (crc & 0x0001)
            {
                crc >>= 1;
                crc ^= 0xA001;
            }
            else
            {
                crc >>= 1;
            }
        }
    }
    return crc;
}
