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

    // Spin-block on the calling thread. This is intentional: refreshDevices()
    // is a sequential UI operation; 3-5 seconds of blocking is acceptable and
    // dramatically simpler than a signal/slot dance with a QEventLoop.
    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
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
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        disconnectDevice();
    }

    return ok;
}

// ---------------------------------------------------------------------------
// Phase 3: sendSpeedCommand (low-latency fire-and-forget)
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

    // Increment the in-flight counter BEFORE dispatch so disconnectDevice can
    // wait for the write to settle.
    pimpl->inFlightWrites.fetch_add(1, std::memory_order_release);

    try
    {
        auto* selfRaw = this;
        auto asyncOp = tx.WriteValueAsync(buffer, GattWriteOption::WriteWithoutResponse);
        // The WinRT delegate invokes operator() const. const_cast the pointer
        // so we can touch pimpl's atomic counter from a const context. This is
        // safe: the counter is an atomic and the manager outlives the async op
        // (see disconnectDevice() which drains inFlightWrites before destroying
        // pimpl).
        asyncOp.Completed([selfRaw](winrt::Windows::Foundation::IAsyncOperation<
            winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::GattCommunicationStatus> const&,
            winrt::Windows::Foundation::AsyncStatus status)
        {
            auto* selfNC = const_cast<WinRtBleManager*>(selfRaw);
            selfNC->pimpl->inFlightWrites.fetch_sub(1, std::memory_order_release);
            if (status != winrt::Windows::Foundation::AsyncStatus::Completed)
            {
                qWarning() << "sendSpeedCommand: BLE write failed (status:"
                           << (int)status << ")";
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
// Teardown
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
    }

    // 2. If we were really connected and some write is still in flight, wait
    // up to 500ms for WriteValueAsync coroutines to land their completion
    // callback. That callback (see sendSpeedCommand above) only touches
    // the manager's inFlightWrites counter, which lives in pimpl and stays
    // valid until the next step destroys `pimpl`. 500ms comfortably covers
    // a BLE link-level timeout on Windows (typical ~300-500ms).
    if (wasConnected)
    {
        const auto begin = std::chrono::steady_clock::now();
        const auto deadline = begin + std::chrono::milliseconds(500);
        while (pimpl->inFlightWrites.load(std::memory_order_acquire) > 0
               && std::chrono::steady_clock::now() < deadline)
        {
            std::this_thread::yield();
        }
        const int remaining = pimpl->inFlightWrites.load(std::memory_order_relaxed);
        if (remaining > 0)
        {
            qWarning() << "disconnectDevice: " << remaining
                       << "write(s) still in flight after 500ms drain — abandoning";
        }
    }

    // 3. Clear the ConnectionParametersChanged subscription token. We do
    // NOT need to explicitly revoke it: dropping `pimpl->device` in
    // step 4 destroys the COM object, which auto-detaches all its
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

    // 4. Release the BluetoothLEDevice — drops the COM reference and asks
    // Windows to close the physical BLE link, freeing it for Intiface.
    {
        std::lock_guard l(m_mtx);
        pimpl->device = nullptr;
    }

    if (wasConnected)
    {
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
