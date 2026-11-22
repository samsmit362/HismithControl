// FredorchF21sController.cpp
//
// Implementation of the Fredorch F21S / T8 Program Mode controller.
//
// All code, logs, comments are in ENGLISH per project spec
// (.roo/rules/agents.md).

#include "FredorchF21sController.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <iomanip>
#include <sstream>

#include <QDebug>
#include <QString>

// ---------------------------------------------------------------------------
// Modbus RTU register constants (Program Mode)
// ---------------------------------------------------------------------------
namespace
{
    constexpr uint16_t REG_MODE_SELECT  = 0x0064;  // 0=pattern, 1=program, 2=free
    constexpr uint16_t REG_PLAY_RECORD  = 0x0069;  // 1=play, 0=record/stop
    constexpr uint16_t REG_LOOP         = 0x006A;  // 1=loop on, 0=loop off
    constexpr uint16_t REG_STEP_BLOCK   = 0x006B;  // step block start (5 regs)
    constexpr uint16_t REG_SPEED        = 0x0066;  // speed adjust (Pattern mode)
    constexpr uint16_t REG_RANGE        = 0x0067;  // range adjust (Pattern mode)
    constexpr uint16_t REG_PATTERN      = 0x0068;  // pattern select (Pattern mode)
    // Register stride between consecutive multi-step program slots.
    // Probe log:
    //   stride +5  (0x006B, 0x0070, ...) — FAILED on HW: 0x0070 is a
    //              service/state register; overwriting it caused wild fast
    //              out-and-back repeats.
    //   stride +6  (0x006B, 0x0071, ...) — CONFIRMED ON HW (2026-11-23):
    //              2-step and 10-step splines run cleanly; 0x0070 stays
    //              untouched as the service separator register.
    constexpr int STEP_SLOT_STRIDE      = 6;
    // All-zero terminator slot (repeat = 0) after the last step.
    // Still DISABLED: with Loop ON the firmware cycles slots forever,
    // so the ping-pong scheme needs no terminator at all.
    constexpr bool STEP_TERMINATOR_ON   = false;
    // Maximum number of program slots reachable over BLE (HW-verified:
    // up to 10 slots at a time). slotOffset must satisfy
    // slotOffset + steps.size() <= MAX_STEP_SLOTS.
    constexpr int  MAX_STEP_SLOTS       = 10;
    }

// ---------------------------------------------------------------------------
// Construction / Destruction
// ---------------------------------------------------------------------------
FredorchF21sController::FredorchF21sController(WinRtBleManager* bleManager,
                                                Config config)
    : m_bleManager(bleManager)
    , m_config(config)
{
}

FredorchF21sController::~FredorchF21sController()
{
    disconnect();
}

// ---------------------------------------------------------------------------
// describeFrame — human-readable one-line description of a Modbus RTU frame
// ---------------------------------------------------------------------------
namespace
{
    std::string regHex(uint16_t v)
    {
        std::ostringstream o;
        o << "0x" << std::uppercase << std::hex
          << std::setw(4) << std::setfill('0') << v;
        return o.str();
    }
}

std::string FredorchF21sController::describeFrame(const std::vector<uint8_t>& frame)
{
    if (frame.size() < 2)
    {
        return "UNKNOWN frame";
    }
    const uint8_t fn = frame[1];

    if (fn == 0x06 && frame.size() >= 6)
    {
        const uint16_t reg = (static_cast<uint16_t>(frame[2]) << 8) | frame[3];
        const uint16_t val = (static_cast<uint16_t>(frame[4]) << 8) | frame[5];
        switch (reg)
        {
            case 0x0064:
                return "Mode Select = " + std::to_string(val)
                     + (val == 1 ? " (PROGRAM)"
                        : val == 2 ? " (FREE)" : " (PATTERN/RF-remote)");
            case 0x0069:
                return "Play/Record = " + std::to_string(val)
                     + (val ? " (PLAY ON)" : " (STOP)");
            case 0x006A:
                return "Loop = " + std::to_string(val) + (val ? " (ON)" : " (OFF)");
            default:
                return "Write reg " + regHex(reg) + " = " + std::to_string(val);
        }
    }

    if (fn == 0x10 && frame.size() >= 19)
    {
        const uint16_t reg = (static_cast<uint16_t>(frame[2]) << 8) | frame[3];
        const uint16_t qty = (static_cast<uint16_t>(frame[4]) << 8) | frame[5];
        std::ostringstream d;
        d << "Write " << qty << " regs @" << regHex(reg);
        if (reg >= 0x006B && ((reg - 0x006B) % STEP_SLOT_STRIDE == 0))
        {
            const int slot = (reg - 0x006B) / STEP_SLOT_STRIDE;
            const uint16_t spdOut = (static_cast<uint16_t>(frame[7]) << 8)  | frame[8];
            const uint16_t spdIn  = (static_cast<uint16_t>(frame[9]) << 8)  | frame[10];
            const uint16_t posIn  = (static_cast<uint16_t>(frame[11]) << 8) | frame[12];
            const uint16_t posOut = (static_cast<uint16_t>(frame[13]) << 8) | frame[14];
            const uint16_t rep    = (static_cast<uint16_t>(frame[15]) << 8) | frame[16];
            d << " — step slot " << slot
              << ": speedOut=" << spdOut << ", speedIn=" << spdIn
              << ", pos " << posIn << "->" << posOut
              << ", repeat=" << rep
              << (rep == 0 ? " (STOP MARKER)" : "");
        }
        return d.str();
    }

    if (fn == 0x03 && frame.size() >= 6)
    {
        // fn 0x03 is a Modbus READ REQUEST — sent to the device over a
        // BLE TX write, hence the log line reads:
        //   [BLE WRITE] ... SEND read request (fn 0x03): ...
        const uint16_t reg = (static_cast<uint16_t>(frame[2]) << 8) | frame[3];
        if (reg == 0x0069)
        {
            return "SEND read request (fn 0x03): Play/Record state (0x0069)";
        }
        return "SEND read request (fn 0x03): reg " + regHex(reg);
    }

    return "fn " + regHex(fn);
}

// ---------------------------------------------------------------------------
// logFrame — log one frame: "[BLE WRITE] Fredorch: <description> (hex=...)"
// ---------------------------------------------------------------------------
void FredorchF21sController::logFrame(const std::vector<uint8_t>& frame) const
{
    std::ostringstream hex;
    for (size_t i = 0; i < frame.size(); ++i)
    {
        if (i > 0) hex << ' ';
        hex.width(2);
        hex.fill('0');
        hex << std::uppercase << std::hex << static_cast<int>(frame[i]);
    }
    qDebug().noquote() << "[BLE WRITE] Fredorch:"
                       << describeFrame(frame)
                       << "(hex=" << hex.str() << ")";
}

// ---------------------------------------------------------------------------
// lastError
// ---------------------------------------------------------------------------
QString FredorchF21sController::lastError() const
{
    std::lock_guard l(m_lastErrorMtx);
    return QString::fromStdString(m_lastError);
}

void FredorchF21sController::setLastError(const std::string& s)
{
    std::lock_guard l(m_lastErrorMtx);
    m_lastError = s;
}

// ---------------------------------------------------------------------------
// connect
// ---------------------------------------------------------------------------
bool FredorchF21sController::connect()
{
    return connectWithAddress(m_config.deviceAddress);
}

bool FredorchF21sController::connectWithAddress(uint64_t deviceAddress)
{
    if (!m_bleManager)
    {
        setLastError("bleManager is null");
        return false;
    }
    if (deviceAddress == 0)
    {
        setLastError("deviceAddress is 0");
        return false;
    }

    // Idempotent: disconnect first if already connected.
    if (m_connected.load())
    {
        disconnect();
    }

    // Step 1: GATT handshake (binds FFB1 TX, subscribes FFB2 notify).
    bool ok = m_bleManager->connectToFredorch(deviceAddress,
                                              m_config.connectTimeoutMs);
    if (!ok)
    {
        std::string msg = "connectToFredorch failed: "
            + m_bleManager->lastHandshakeError().toStdString();
        setLastError(msg);
        qWarning().noquote() << "FredorchF21sController::connect:" << msg;
        return false;
    }

    m_connected.store(true, std::memory_order_release);

    // Step 1b: Register the raw-notify callback so readPlayState() can
    // capture Modbus responses from the FFB2 notify characteristic.
    m_bleManager->setRawNotifyCallback(
        [this](const uint8_t* data, size_t len) {
            {
                std::lock_guard l(m_playStateMtx);
                m_lastNotifyBytes.assign(data, data + len);
            }
            m_notifySeq.fetch_add(1, std::memory_order_release);
        });

    // Step 2: Queue the Program Mode init sequence on the worker thread
    //   (avoids blocking the calling thread for ~120ms of BLE writes).
    {
        std::lock_guard l(m_slot.mtx);
        m_slot.pendingInit = true;
    }

    // Step 3: Start the worker thread (it will pick up pendingInit immediately).
    startWorker();
    m_slot.cv.notify_one();

    // Step 4: Post-connect settle. The F21S firmware (YXLinksSPP bridge)
    // needs a brief grace period after the GATT link comes up before it
    // reliably accepts and applies the first Program Mode commands.
    // Without this, the very first moveTo after a (re)connect fails
    // intermittently ("works every other session").
    //   4a. Wait until all init frames have been flushed to the radio
    //       (in-flight counter back to 0), bounded to 2s.
    const auto flushDeadline = std::chrono::steady_clock::now()
                              + std::chrono::milliseconds(2000);
    while (m_bleManager->inFlightWriteCount() > 0
           && std::chrono::steady_clock::now() < flushDeadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    //   4b. Fixed 150ms settle: covers the internal UART transfer of the
    //       4 init frames + MCU Modbus parsing + firmware state machine
    //       entering Program Mode. Empirically required for the first
    //       moveTo to be accepted on a cold connect.
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    qDebug().noquote() << "FredorchF21sController::connect: SUCCESS"
                       << "(device:" << QString::fromStdString(m_config.deviceName)
                       << ", address: 0x" << QString::number(deviceAddress, 16) << ")";
    return true;
}

// ---------------------------------------------------------------------------
// disconnect
// ---------------------------------------------------------------------------
void FredorchF21sController::disconnect()
{
    const bool wasConnected = m_connected.load(std::memory_order_acquire);

    // 1. Stop the worker FIRST to avoid concurrent frame dispatch.
    //    (sendRawFrameAndWait pumps Qt events — not safe while worker is active)
    stopWorker();

    // 2. If was connected, send zero-step + Play OFF via fire-and-forget
    //    (no event pumping, no race with worker).
    //    In PLAY+LOOP mode the device replays the step block forever, so
    //    replacing it with a no-op step (speed=0, pos=0) stops the motor.
    if (wasConnected && m_bleManager && m_bleManager->isConnected())
    {
        auto zeroStepFrame = buildStepBlock(0, 0, 0, 0, 1);
        logFrame(zeroStepFrame);
        m_bleManager->sendRawFrame(zeroStepFrame);
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        auto stopFrame = buildStopFrame();
        logFrame(stopFrame);
        m_bleManager->sendRawFrame(stopFrame);
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        // Return control to the RF remote (Pattern mode, 0x0064=0).
        auto modeFrame = buildWriteSingleRegister(REG_MODE_SELECT, 0x0000);
        logFrame(modeFrame);
        m_bleManager->sendRawFrame(modeFrame);
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        qDebug() << "FredorchF21sController::disconnect: zeroStep + stop + mode frames sent";
    }

    // 3. Tear down the GATT link.
    if (m_bleManager)
    {
        m_bleManager->disconnectDevice();
    }

    m_connected.store(false, std::memory_order_release);

    // Clear the raw-notify callback so the BLE manager doesn't call
    // into a half-destroyed controller.
    if (m_bleManager)
    {
        m_bleManager->setRawNotifyCallback(nullptr);
    }

    qDebug() << "FredorchF21sController::disconnect: done";
}

// ---------------------------------------------------------------------------
// Worker thread lifecycle
// ---------------------------------------------------------------------------
void FredorchF21sController::startWorker()
{
    std::lock_guard l(m_threadMtx);

    if (m_workerThread.joinable())
    {
        m_workerThread.join();
    }

    m_shutdown.store(false, std::memory_order_release);
    m_workerThread = std::thread(&FredorchF21sController::workerLoop, this);
}

void FredorchF21sController::stopWorker()
{
    std::lock_guard l(m_threadMtx);

    m_shutdown.store(true, std::memory_order_release);
    m_slot.cv.notify_one();

    if (m_workerThread.joinable())
    {
        m_workerThread.join();
    }
}

// ---------------------------------------------------------------------------
// Worker loop
//
// The worker thread is the sole consumer of the CommandSlot. It:
//   1. Waits for a command (pendingStop / pendingMove / pendingInit).
//   2. Processes the highest-priority command:
//      - pendingStop  -> send 0x0069=0 (stop)
//      - pendingMove  -> send step frame, re-check generation, send play frame
//      - pendingInit  -> send the Program Mode init sequence
//   3. Throttles move commands to >= throttleMs apart.
//
// Race protection:
//   - stop() increments generation, erases pendingMove, sets pendingStop.
//   - Between the step frame and the play frame of a moveTo, the worker
//     re-checks generation. If a stop() landed in between, the play frame
//     is suppressed and a stop frame is sent instead.
// ---------------------------------------------------------------------------
void FredorchF21sController::workerLoop()
{
    // Throttle bookkeeping: the last time a MOVE command was dispatched.
    // Stop and Init are NOT throttled.
    auto lastMoveTime = std::chrono::steady_clock::time_point{};

    std::unique_lock<std::mutex> lock(m_slot.mtx);

    while (!m_shutdown.load(std::memory_order_acquire))
    {
        // Wait for any pending command or shutdown signal.
        m_slot.cv.wait_for(lock, std::chrono::milliseconds(200), [this] {
            return m_shutdown.load(std::memory_order_acquire)
                || m_slot.pendingStop
                || m_slot.pendingInit
                || m_slot.pendingProgramMode
                || m_slot.pendingSteps.has_value()
                || m_slot.pendingLoopRange;
        });

        if (m_shutdown.load(std::memory_order_acquire))
        {
            break;
        }

        // -----------------------------------------------------------------
        // Priority 1: STOP (high priority, not throttled)
        // -----------------------------------------------------------------
        if (m_slot.pendingStop)
        {
            m_slot.pendingStop = false;
            const uint64_t gen = m_slot.generation;
            lock.unlock();

            // Stop strategy (matches buttplug handle_output_oscillate_cmd speed=0):
            //   1. Replace the step block with a no-op (speed=0, pos=0, pos=0).
            //      In PLAY+LOOP mode the device replays this step forever, so
            //      a zero-speed/zero-distance step effectively halts the motor.
            //   2. Play OFF (0x69=0) as a backup.
            auto zeroStepFrame = buildStepBlock(0, 0, 0, 0, 1);
            sendFrameInternal(zeroStepFrame);
            auto stopFrame = buildStopFrame();
            sendFrameInternal(stopFrame);
            // 3. Return control to the RF remote: select Pattern mode
            //    (0x0064 = 0). The device now listens to the RF remote
            //    until the next moveTo re-enters Program Mode.
            auto modeFrame = buildWriteSingleRegister(REG_MODE_SELECT, 0x0000);
            sendFrameInternal(modeFrame);
            m_lastTx = std::chrono::steady_clock::now();
            // The motor is now halted — clear the play/loop state so the
            // next moveTo uses the full Play OFF -> write -> Play ON path.
            m_playing.store(false);
            m_lastLoopOn.store(false);
            qDebug().noquote() << "Fredorch worker: STOP (zeroStep + playOff) sent (gen="
                               << gen << ")";

            lock.lock();
            continue;
        }

        // -----------------------------------------------------------------
        // Priority 2: INIT (Program Mode init sequence, not throttled)
        // -----------------------------------------------------------------
        if (m_slot.pendingInit)
        {
            m_slot.pendingInit = false;
            lock.unlock();

            sendInitSequence();

            lock.lock();
            continue;
        }

        // -----------------------------------------------------------------
        // Priority 2a: EXPLICIT PROGRAM MODE (single frame 0x0064=1)
        //   Called by the user via startProgramMode() before the first
        //   moveTo() after a stop()/disconnect() that returned the device
        //   to Pattern mode (RF remote). Not auto-sent by moveTo.
        // -----------------------------------------------------------------
        if (m_slot.pendingProgramMode)
        {
            m_slot.pendingProgramMode = false;
            lock.unlock();

            auto modeFrame = buildWriteSingleRegister(REG_MODE_SELECT, 0x0001);
            const bool ok = sendFrameInternal(modeFrame);
            qDebug().noquote() << "Fredorch worker: Program Mode ON (0x0064=1), ok="
                               << (ok ? "yes" : "NO");

            lock.lock();
            continue;
        }

        // -----------------------------------------------------------------
        // Priority 2b: LOOP RANGE (Intiface-style A<->B oscillation)
        //   Sends: Loop ON -> step block (A->B) -> Play.
        //   The firmware then oscillates forever until stop().
        // -----------------------------------------------------------------
        if (m_slot.pendingLoopRange)
        {
            m_slot.pendingLoopRange = false;
            const int a  = m_slot.loopPosA;
            const int b  = m_slot.loopPosB;
            const int spd = m_slot.loopSpeed;
            const uint64_t gen = m_slot.generation;
            lock.unlock();

            // 1. Loop ON: 01 06 00 6A 00 01 [CRC]
            auto loopFrame = buildWriteSingleRegister(REG_LOOP, 0x0001);
            bool ok1 = sendFrameInternal(loopFrame);

            // 2. Step block: move from posA to posB at speed
            auto stepFrame = buildStepBlock(static_cast<uint8_t>(spd),
                                            static_cast<uint8_t>(spd),
                                            static_cast<uint16_t>(a),
                                            static_cast<uint16_t>(b),
                                            1);
            bool ok2 = sendFrameInternal(stepFrame);

            // 3. Play: 01 06 00 69 00 01 [CRC]
            auto playFrame = buildPlayFrame();
            bool ok3 = sendFrameInternal(playFrame);

            if (ok3)
            {
                m_playing.store(true);
                m_lastLoopOn.store(true);
            }

            qDebug().noquote() << "Fredorch worker: LOOP RANGE"
                               << "(A=" << a << ", B=" << b
                               << ", speed=" << spd
                               << ", gen=" << gen
                               << ", ok=" << (ok1 && ok2 && ok3 ? "yes" : "NO") << ")";

            lastMoveTime = std::chrono::steady_clock::now();
            m_lastTx = std::chrono::steady_clock::now();
            lock.lock();
            continue;
        }

        // -----------------------------------------------------------------
        // Priority 3: MOVE — batch (throttled)
        // -----------------------------------------------------------------
        if (!m_slot.pendingSteps.has_value())
        {
            // No pending command — BLE keep-alive check. While a looping
            // program is playing the device emits no traffic, and the BLE
            // bridge drops into power-save after ~1s of silence. If more
            // than 1s has passed since the last frame left the radio, poke
            // it with a Modbus read of the Play state register (0x0069) —
            // the request frame alone keeps the link awake; the reply value
            // is discarded.
            if (m_playing.load(std::memory_order_acquire)
                && m_lastLoopOn.load(std::memory_order_acquire)
                && m_lastTx != std::chrono::steady_clock::time_point{})
            {
                const auto nowKa = std::chrono::steady_clock::now();
                const auto idleMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                    nowKa - m_lastTx).count();
                if (idleMs >= 1000)
                {
                    m_lastTx = nowKa;
                    lock.unlock();
                    // Fire-and-forget BLE keep-alive ping: send a Modbus
                    // read request (fn 0x03, reg 0x0069) and wait 30 ms for
                    // it to leave the radio. The request frame alone keeps
                    // the BLE bridge awake; we do NOT wait for the FFB2
                    // notify reply (which may never arrive if the UI thread
                    // is busy pumping a while(1) loop).
                    std::vector<uint8_t> ping;
                    ping.reserve(8);
                    ping.push_back(0x01);
                    ping.push_back(0x03);
                    ping.push_back(0x00);
                    ping.push_back(0x69);
                    ping.push_back(0x00);
                    ping.push_back(0x01);
                    const uint16_t crc = modbusCrc16(ping.data(), ping.size());
                    ping.push_back(static_cast<uint8_t>(crc & 0xFF));
                    ping.push_back(static_cast<uint8_t>((crc >> 8) & 0xFF));
                    logFrame(ping);
                    const bool sent = m_bleManager->sendRawFrameQueued(ping);
                    std::this_thread::sleep_for(std::chrono::milliseconds(30));
                    qDebug().noquote() << "Fredorch worker: BLE keep-alive ping"
                                       << "(idle=" << idleMs << "ms"
                                       << ", sent=" << (sent ? "yes" : "NO") << ")";
                    lock.lock();
                }
            }
            continue;
        }

        // Throttle check: if the last move was too recent, wait.
        const auto now = std::chrono::steady_clock::now();
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - lastMoveTime).count();
        const int throttleMs = m_config.throttleMs;

        if (lastMoveTime != std::chrono::steady_clock::time_point{}
            && elapsed < throttleMs)
        {
            // Not enough time has passed — wait the remaining time.
            const auto remaining = std::chrono::milliseconds(throttleMs - elapsed);
            m_slot.cv.wait_for(lock, remaining, [this] {
                return m_shutdown.load(std::memory_order_acquire)
                    || m_slot.pendingStop;
            });

            // If a stop or shutdown arrived while we were waiting, re-loop
            // to handle it first.
            if (m_shutdown.load(std::memory_order_acquire) || m_slot.pendingStop)
            {
                continue;
            }
            // Otherwise, proceed with the (possibly updated) pendingSteps.
        }

        // Extract the batch (latest-wins) and clear it so we don't
        // re-process the same command on the next loop iteration.
        auto steps = std::move(*m_slot.pendingSteps);
        const bool loop = m_slot.pendingLoop;
        const int slotOffset = m_slot.pendingSlotOffset;
        const uint64_t gen = m_slot.generation;
        m_slot.pendingSteps.reset();
        m_slot.pendingLoop = false;
        m_slot.pendingSlotOffset = 0;
        lock.unlock();

        // Ping-pong (LIVE REWRITE) mode: if the device is currently playing
        // a LOOPING program, the firmware accepts fn 0x10 writes to the slot
        // table on the fly (no Play OFF needed). The rewritten slots take
        // effect on the next lap of the loop — this is what allows chaining
        // 10-step splines back-to-back with zero dead time.
        const bool liveRewrite = m_playing.load(std::memory_order_acquire)
                               && m_lastLoopOn.load(std::memory_order_acquire);

        // --- Phase 1: RESET (Play OFF + settle) — skipped in live mode ---
        // In Program mode, the firmware ignores writes to 0x006B..0x0079
        // while Play (0x69) is active. We must send Play OFF first and wait
        // for the motor to physically stop before the batch 0x10 frame
        // lands. The stopSettleMs (default 50 ms) covers:
        //   - the BLE→UART transfer of the 8-byte Play OFF frame (~12 ms)
        //   - the MCU's Modbus parse + state machine transition
        //   - the physical motor deceleration to a full halt
        //   - the firmware unlocking the program-memory window
        if (!liveRewrite)
        {
            auto playOffFrame = buildStopFrame();
            sendFrameInternal(playOffFrame);
            std::this_thread::sleep_for(std::chrono::milliseconds(m_config.stopSettleMs));

            // Check generation: if stop() or a newer batch arrived during the
            // Play OFF + settle window, abort the batch write.
            bool aborted = false;
            {
                std::lock_guard l(m_slot.mtx);
                if (m_slot.generation != gen || m_slot.pendingStop)
                {
                    aborted = true;
                }
            }
            if (aborted)
            {
                qDebug().noquote() << "Fredorch worker: batch ABORTED after Play OFF"
                                   << "(gen=" << gen << ", steps=" << steps.size() << ")";
                lock.lock();
                continue;
            }
        }

        // --- Phase 2: BATCH WRITE (multi-step program slots) ---
        // The F21S keeps program slots in register space starting at 0x006B.
        // Each slot is a 5-register block:
        //   [speedOut] [speedIn] [posIn] [posOut] [repeat]
        // Slot i is addressed at 0x006B + (slotOffset + i) * STEP_SLOT_STRIDE
        // (stride +6: slots at 0x006B, 0x0071, 0x0077, 0x007D, ...).
        //
        // Firmware execution model (stpihkal #131 + HW tests):
        //   - the program runs slot by slot in address order;
        //   - a slot with repeat == 0 is the STOP MARKER: the firmware
        //     halts the program there without executing the slot;
        //   - a slot with repeat != 0 executes, then the firmware advances
        //     to the next slot (a repeat != 0 in a trailing garbage slot
        //     is what caused the old "7 wild repeats" bug).
        //
        // Batch mapping (see buildBatchStepTableFrame):
        //   N steps -> N slots, each carrying all four parameters:
        //     [speed1->speedOut] [speed2->speedIn] [pos1->posIn] [pos2->posOut]
        //     repeat = 1. A zero pos/speed is a valid value sent as-is.
        //   + one all-zero terminator slot with repeat = 0 (if enabled).
        //
        // Sequence:
        //   a) Loop (0x06, 0x6A = loop ? 1 : 0)
        //   b) N step-slot frames (0x10, 5 registers each)
        //      (+ terminator frame for N >= 2)
        //   c) Play ON (0x06, 0x69 = 1).
        // Loop control: sent only for a (re)start. In live mode the loop
        // state is left untouched — the firmware is already cycling.
        bool okLoop = true;
        if (!liveRewrite)
        {
            auto loopFrame = buildWriteSingleRegister(REG_LOOP, loop ? 0x0001 : 0x0000);
            okLoop = sendFrameInternal(loopFrame);
        }

        auto tableFrames = buildBatchStepTableFrame(steps, slotOffset);
        bool okTable = true;
        for (const auto& f : tableFrames)
        {
            okTable = okTable && sendFrameInternal(f);
        }

        // Play ON: sent only for a (re)start. In live mode Play stays ON and
        // the rewritten slots are picked up on the next lap.
        bool okPlay = true;
        if (!liveRewrite)
        {
            auto playOnFrame = buildPlayFrame();
            okPlay = sendFrameInternal(playOnFrame);
        }
        const bool batchOk = okLoop && okTable && okPlay;

        // Track the play/loop state (only changes on a (re)start).
        if (!liveRewrite)
        {
            m_playing.store(batchOk);
            m_lastLoopOn.store(batchOk && loop);
        }

        // Debug log: hex dump of every frame in the batch
        {
            auto hexOf = [](const std::vector<uint8_t>& f) {
                std::ostringstream hex;
                for (size_t i = 0; i < f.size(); ++i)
                {
                    if (i > 0) hex << ' ';
                    hex.width(2);
                    hex.fill('0');
                    hex << std::uppercase << std::hex << static_cast<int>(f[i]);
                }
                return hex.str();
            };
            std::ostringstream hexAll;
            for (size_t i = 0; i < tableFrames.size(); ++i)
            {
                if (i > 0) hexAll << " | ";
                hexAll << hexOf(tableFrames[i]);
            }
            qDebug().noquote() << "[BLE WRITE] Fredorch: BATCH frames"
                               << "(steps=" << steps.size()
                               << ", frames=" << tableFrames.size()
                               << ", offset=" << slotOffset
                               << ", loop=" << (loop ? "ON" : "off")
                               << ", live=" << (liveRewrite ? "REWRITE" : "no")
                               << ", gen=" << gen
                               << ", ok=" << (batchOk ? "yes" : "NO")
                               << ", hex=" << hexAll.str() << ")";
        }

        if (!batchOk)
        {
            lock.lock();
            continue;
        }

        lastMoveTime = std::chrono::steady_clock::now();
        m_lastTx = std::chrono::steady_clock::now();
        lock.lock();
    }

    qDebug() << "Fredorch worker: thread exiting";
}

// ---------------------------------------------------------------------------
// moveTo (single-step convenience overload)
// ---------------------------------------------------------------------------
bool FredorchF21sController::moveTo(int targetPos, int speed)
{
    return moveTo({ { targetPos, speed, targetPos, speed } }, /*slotOffset=*/0, /*loop=*/false);
}

// ---------------------------------------------------------------------------
// moveTo (batch form, slotOffset = 0 — full (re)start sequence)
// ---------------------------------------------------------------------------
bool FredorchF21sController::moveTo(const std::vector<FredorchMoveSteps>& steps, bool loop)
{
    return moveTo(steps, /*slotOffset=*/0, loop);
}

// ---------------------------------------------------------------------------
// moveTo (batch / spline form with explicit slot offset)
//
// slotOffset: 0..9 — first program slot to program (slot i is written at
//             0x006B + (slotOffset + i) * STEP_SLOT_STRIDE).
// When the device is already playing a looping program (Loop ON + Play ON),
// the worker rewrites the given slots LIVE (no Play OFF, no Play ON) — a
// seamless ping-pong update picked up on the next lap. The `loop` flag is
// ignored in live mode.
// ---------------------------------------------------------------------------
bool FredorchF21sController::moveTo(const std::vector<FredorchMoveSteps>& steps,
                                    int slotOffset, bool loop)
{
    if (!m_connected.load(std::memory_order_acquire))
    {
        return false;
    }
    if (steps.empty())
    {
        qWarning() << "FredorchF21sController::moveTo: empty steps vector";
        return false;
    }
    if (slotOffset < 0
        || slotOffset > (MAX_STEP_SLOTS - 1)
        || slotOffset + static_cast<int>(steps.size()) > MAX_STEP_SLOTS)
    {
        qWarning() << "FredorchF21sController::moveTo: slot range out of bounds"
                   << "(offset=" << slotOffset << ", steps=" << steps.size()
                   << ", max slots=" << MAX_STEP_SLOTS << ")";
        return false;
    }

    // Quantize and clamp each step into the program-memory register format.
    // Note: zero pos2/speed2 are valid values — they are sent to the
    // registers as-is and are NEVER replaced by pos1/speed1.
    std::vector<FredorchMoveSteps> normalized;
    normalized.reserve(steps.size());
    for (const auto& s : steps)
    {
        normalized.push_back(FredorchMoveSteps{
            quantizePosition(s.pos1),
            clampSpeed(s.speed1),
            quantizePosition(s.pos2),
            clampSpeed(s.speed2)
        });
    }

    {
        std::lock_guard l(m_slot.mtx);
        // Latest-wins: overwrite any previous pending batch.
        m_slot.pendingSteps = std::move(normalized);
        m_slot.pendingLoop = loop;
        m_slot.pendingSlotOffset = slotOffset;
        m_slot.generation++;
        m_slot.pendingStop = false;  // a new move cancels a pending stop
    }
    m_slot.cv.notify_one();

    return true;
}

// ---------------------------------------------------------------------------
// startLoopRange — Intiface-style continuous A<->B oscillation
//
// Mirrors buttplug fredorch.rs handle_output_oscillate_cmd: program a
// step block that moves from posA to posB at the given speed, turn the
// Loop register ON, and Play. The firmware then oscillates A<->B
// forever (loop) until a stop() is issued.
// ---------------------------------------------------------------------------
bool FredorchF21sController::startLoopRange(int posA, int posB, int speed)
{
    if (!m_connected.load(std::memory_order_acquire))
    {
        return false;
    }

    const int a = quantizePosition(posA);
    const int b = quantizePosition(posB);
    const int spd = clampSpeed(speed);

    {
        std::lock_guard l(m_slot.mtx);
        m_slot.pendingLoopRange = true;
        m_slot.loopPosA = a;
        m_slot.loopPosB = b;
        m_slot.loopSpeed = spd;
        m_slot.generation++;
        m_slot.pendingStop = false;
        m_slot.pendingSteps.reset();
    }
    m_slot.cv.notify_one();

    return true;
}

// ---------------------------------------------------------------------------
// startProgramMode — explicitly re-enter Program Mode (single frame 0x0064=1)
//
// stop() and disconnect() return the device to Pattern mode (0x0064=0,
// RF-remote control). Call this before the first moveTo() batch after such
// a stop so the step-table writes are accepted again. moveTo() itself never
// auto-sends the mode frame — it keeps the frame count minimal for the
// ping-pong live-rewrite path.
// ---------------------------------------------------------------------------
bool FredorchF21sController::startProgramMode()
{
    if (!m_connected.load(std::memory_order_acquire))
    {
        return false;
    }

    {
        std::lock_guard l(m_slot.mtx);
        m_slot.pendingProgramMode = true;
        m_slot.generation++;
        m_slot.pendingStop = false;
        m_slot.pendingSteps.reset();
        m_slot.pendingLoopRange = false;
    }
    m_slot.cv.notify_one();

    return true;
}

// ---------------------------------------------------------------------------
// stop
// ---------------------------------------------------------------------------
bool FredorchF21sController::stop()
{
    if (!m_connected.load(std::memory_order_acquire))
    {
        return false;
    }

    {
        std::lock_guard l(m_slot.mtx);
        // Invalidate any in-flight or pending moveTo / loopRange.
        m_slot.generation++;
        m_slot.pendingSteps.reset();
        m_slot.pendingLoop = false;
        m_slot.pendingLoopRange = false;
        m_slot.pendingStop = true;
    }
    m_slot.cv.notify_one();

    return true;
}

// ---------------------------------------------------------------------------
// sendFrame (raw, bypasses worker and throttle)
// ---------------------------------------------------------------------------
bool FredorchF21sController::sendFrame(const std::vector<uint8_t>& frame)
{
    if (!m_bleManager || !m_bleManager->isConnected())
    {
        return false;
    }
    logFrame(frame);
    return m_bleManager->sendRawFrameAndWait(frame, m_config.frameTimeoutMs);
}

// ---------------------------------------------------------------------------
// readPlayState — blocking Modbus fn 0x03 read of register 0x0069
//
// Request : 01 03 00 69 00 01 [CRC_LO] [CRC_HI]  (8 bytes)
// Response (FFB2 notify): 01 03 02 [ValHi] [ValLo] [CRC_LO] [CRC_HI]  (7 bytes)
//   0x0001 = Play ON, 0x0000 = Play OFF.
//
// The FFB2 notify handler (registered in connectWithAddress) stores every
// payload in m_lastNotifyBytes and bumps m_notifySeq. We snapshot the
// sequence, send the read request, then poll for a fresh valid response.
// ---------------------------------------------------------------------------
bool FredorchF21sController::readPlayState(uint16_t* stateOut, int timeoutMs)
{
    if (!stateOut)
    {
        setLastError("readPlayState: stateOut is null");
        return false;
    }
    if (!m_bleManager || !m_bleManager->isConnected())
    {
        setLastError("readPlayState: not connected");
        return false;
    }

    // 1. Snapshot the notification sequence so we only accept FRESH replies.
    const uint64_t seqBefore = m_notifySeq.load(std::memory_order_acquire);

    // 2. Build and send the Modbus Read Holding Registers frame (fn 0x03):
    //    01 03 00 69 00 01 [CRC_LO] [CRC_HI]
    std::vector<uint8_t> frame;
    frame.reserve(8);
    frame.push_back(0x01);  // unit id
    frame.push_back(0x03);  // function: read holding registers
    frame.push_back(0x00);  // register hi
    frame.push_back(0x69);  // register lo (0x0069 = Play/Record state)
    frame.push_back(0x00);  // quantity hi
    frame.push_back(0x01);  // quantity lo (1 register)
    const uint16_t crc = modbusCrc16(frame.data(), frame.size());
    frame.push_back(static_cast<uint8_t>(crc & 0xFF));
    frame.push_back(static_cast<uint8_t>((crc >> 8) & 0xFF));

    logFrame(frame);
    if (!m_bleManager->sendRawFrameQueued(frame))
    {
        setLastError("readPlayState: sendRawFrameQueued failed");
        return false;
    }
    // Settle: let the frame leave the radio (same 30ms gap as sendFrameInternal).
    std::this_thread::sleep_for(std::chrono::milliseconds(30));

    // 3. Poll for a fresh, valid 7-byte response:
    //    01 03 02 [ValHi] [ValLo] [CRC_LO] [CRC_HI]
    const auto deadline = std::chrono::steady_clock::now()
                         + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (m_notifySeq.load(std::memory_order_acquire) > seqBefore)
        {
            std::vector<uint8_t> rx;
            {
                std::lock_guard l(m_playStateMtx);
                rx = m_lastNotifyBytes;
            }
            if (rx.size() >= 7
                && rx[0] == 0x01
                && rx[1] == 0x03
                && rx[2] == 0x02)
            {
                // Verify CRC over the first 5 bytes.
                const uint16_t rxCrc = modbusCrc16(rx.data(), 5);
                if (static_cast<uint8_t>(rxCrc & 0xFF) == rx[5]
                    && static_cast<uint8_t>((rxCrc >> 8) & 0xFF) == rx[6])
                {
                    *stateOut = (static_cast<uint16_t>(rx[3]) << 8) | rx[4];
                    {
                        std::ostringstream hex;
                        for (size_t i = 0; i < rx.size(); ++i)
                        {
                            if (i > 0) hex << ' ';
                            hex.width(2);
                            hex.fill('0');
                            hex << std::uppercase << std::hex << static_cast<int>(rx[i]);
                        }
                        qDebug().noquote() << "[BLE READ] Fredorch: Play state ="
                                           << (*stateOut ? "PLAY (1)" : "STOP (0)")
                                           << "(hex=" << hex.str() << ")";
                    }
                    return true;
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    setLastError("readPlayState: timeout waiting for FFB2 response"
                 " (BLE bridge may be in power-save mode)");
    qWarning().noquote() << "Fredorch readPlayState: TIMEOUT after"
                         << timeoutMs << "ms — possible BLE power-save";
    return false;
}

// ---------------------------------------------------------------------------
// sendFrameInternal — worker-thread-only frame dispatch (queued, no cancel)
//
// Dispatches the frame via sendRawFrameQueued(), which uses WriteValue
// WithoutResponse WITHOUT LATEST-WINS cancel. The Windows BLE driver queues
// the write and flushes it at the next connection event (15-30ms). Since no
// Cancel() is issued, back-to-back frames in a moveTo sequence (Play OFF +
// Step + Play ON) are not eaten by the coalescing logic.
//
// A short 10ms settle gap between frames covers the internal UART transfer
// time of the F21S bridge chip (8 bytes at 9600 baud ≈ 8.3ms) and gives the
// microcontroller time to parse the Modbus frame before the next one arrives.
//
// Why NOT sendRawFrame (LATEST-WINS): it CANCELs the previous op, which eats
// the prior frame of the sequence before it leaves the radio buffer.
// Why NOT sendRawFrameAndWait: it pumps QCoreApplication::processEvents()
// on the worker thread, but the GATT delegates fire on the UI (STA) thread
// which may be blocked in a while(1) motion-test loop → false timeouts.
// ---------------------------------------------------------------------------
bool FredorchF21sController::sendFrameInternal(const std::vector<uint8_t>& frame)
{
    if (!m_bleManager || !m_bleManager->isConnected())
    {
        return false;
    }
    logFrame(frame);
    if (!m_bleManager->sendRawFrameQueued(frame))
    {
        return false;
    }

    // Dynamic inter-frame gap: the F21S BLE→UART bridge chip forwards
    // bytes to the MCU over an internal UART at 9600 baud. Each byte
    // takes 10 bits (start + 8 data + stop), so the UART transfer time
    // is: frame.size() * 10 / 9600 seconds = frame.size() * 1.042 ms.
    // We add a 3 ms margin for the MCU's Modbus inter-frame timeout
    // (t1.5 ≈ 1.75 ms at 9600) and parser settling.
    //   8-byte frame  →  8.3 + 3 ≈  12 ms
    //  19-byte frame  → 19.8 + 3 ≈  23 ms
    //const int gapMs = std::max(10, static_cast<int>(frame.size() * 10 / 9600.0 * 1000.0) + 3);
    std::this_thread::sleep_for(std::chrono::milliseconds(30));

    return true;
}

// ---------------------------------------------------------------------------
// sendInitSequence — Program Mode init (0x64=1, 0x6A=0, 0x69=0, placeholder step)
//
// Mirrors the buttplug fredorch.rs initializer (mode -> record -> program a
// "move to position 0 at speed 5" placeholder step). Each frame is dispatched
// via sendFrameInternal (fire-and-forget + settle-drain), so the frames are
// serialized: the previous packet has left the radio before the next one is
// dispatched and no inter-frame sleeps are needed.
// ---------------------------------------------------------------------------
void FredorchF21sController::sendInitSequence()
{
    if (!m_bleManager || !m_bleManager->isConnected())
    {
        qWarning() << "FredorchF21sController::sendInitSequence: not connected";
        return;
    }

    // Reset play/loop state: the init sequence ends with Play OFF.
    m_playing.store(false);
    m_lastLoopOn.store(false);

    // 1. Enter Program Mode: 01 06 00 64 00 01 [CRC]
    auto modeFrame = buildWriteSingleRegister(REG_MODE_SELECT, 0x0001);
    bool ok1 = sendFrameInternal(modeFrame);
    qDebug().noquote() << "Fredorch init[1/4]: mode=Program, ok="
                       << (ok1 ? "yes" : "NO");

    // 2. Loop OFF: 01 06 00 6A 00 00 [CRC]
    auto loopFrame = buildWriteSingleRegister(REG_LOOP, 0x0000);
    bool ok2 = sendFrameInternal(loopFrame);
    qDebug().noquote() << "Fredorch init[2/4]: loop=Off, ok="
                       << (ok2 ? "yes" : "NO");

    // 3. Record/Stop state: 01 06 00 69 00 00 [CRC]
    auto recordFrame = buildWriteSingleRegister(REG_PLAY_RECORD, 0x0000);
    bool ok3 = sendFrameInternal(recordFrame);
    qDebug().noquote() << "Fredorch init[3/4]: record/stop, ok="
                       << (ok3 ? "yes" : "NO");

    // 4. Program a placeholder step (pos 0, speed 5) so the program memory
    //    is defined before the first moveTo() (matches the buttplug
    //    "Program the device to move to position 0 at speed 5" init frame).
    auto stepFrame = buildStepFrame(0, 5);
    bool ok4 = sendFrameInternal(stepFrame);
    qDebug().noquote() << "Fredorch init[4/4]: placeholder step (pos=0, speed=5), ok="
                       << (ok4 ? "yes" : "NO");

    if (!ok1 || !ok2 || !ok3 || !ok4)
    {
        qWarning() << "FredorchF21sController::sendInitSequence: one or more frames failed";
    }
}

// ---------------------------------------------------------------------------
// Frame builders (instance methods, use m_config for step encoding choice)
// ---------------------------------------------------------------------------

// Stop frame: 01 06 00 69 00 00 [CRC]
std::vector<uint8_t> FredorchF21sController::buildStopFrame() const
{
    return buildWriteSingleRegister(REG_PLAY_RECORD, 0x0000);
}

// Play frame: 01 06 00 69 00 01 [CRC]
std::vector<uint8_t> FredorchF21sController::buildPlayFrame() const
{
    return buildWriteSingleRegister(REG_PLAY_RECORD, 0x0001);
}

// Step frame — two encodings:
//   Block (default):  01 10 00 6B 00 05 0A [spdOut] [spdIn] [posIn] [posOut] [repeat] [CRC]  (19 bytes)
//                    — the only encoding documented in stpihkal #131.
//   Packed (experimental): 01 06 00 6B [posHi] [speedLo] [CRC]  (8 bytes)
//                    — undocumented; writes only the first register (0x6B) of
//                    the 5-register step block, leaving posIn/posOut stale.
std::vector<uint8_t> FredorchF21sController::buildStepFrame(int pos, int speed) const
{
    if (m_config.useStepBlock)
    {
        // 5-register block (fn 0x10):
        //   speedOut = speed, speedIn = speed,
        //   posIn = pos, posOut = pos (equal endpoints = "move to pos and
        //   hold" — exactly what buttplug fredorch.rs and the stpihkal #131
        //   captures use, e.g. "90 00 90 00" = "Move to 144 at speed 5"),
        //   repeat = 1.
        return buildStepBlock(static_cast<uint8_t>(speed),
                              static_cast<uint8_t>(speed),
                              static_cast<uint16_t>(pos),
                              static_cast<uint16_t>(pos),
                              1);
    }
    else
    {
        // Packed single register (fn 0x06):
        //   Value = (pos << 8) | speed
        //   pos is in the upper byte, speed in the lower byte.
        const uint16_t value = (static_cast<uint16_t>(pos) << 8)
                             | static_cast<uint16_t>(speed);
        return buildWriteSingleRegister(REG_STEP_BLOCK, value);
    }
}

// ---------------------------------------------------------------------------
// Static Modbus helpers
// ---------------------------------------------------------------------------

uint16_t FredorchF21sController::modbusCrc16(const uint8_t* data, size_t len)
{
    // CRC-16/MODBUS polynomial 0xA001 (reflected).
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; ++i)
    {
        crc ^= static_cast<uint16_t>(data[i]);
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

std::vector<uint8_t> FredorchF21sController::buildWriteSingleRegister(
    uint16_t regAddr, uint16_t value)
{
    // Modbus RTU Write Single Register (fn 0x06):
    //   01 06 [RegHi] [RegLo] [ValHi] [ValLo] [CRCLo] [CRCHi]  (8 bytes)
    std::vector<uint8_t> frame;
    frame.reserve(8);
    frame.push_back(0x01);
    frame.push_back(0x06);
    frame.push_back(static_cast<uint8_t>((regAddr >> 8) & 0xFF));
    frame.push_back(static_cast<uint8_t>(regAddr & 0xFF));
    frame.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
    frame.push_back(static_cast<uint8_t>(value & 0xFF));

    const uint16_t crc = modbusCrc16(frame.data(), frame.size());
    frame.push_back(static_cast<uint8_t>(crc & 0xFF));
    frame.push_back(static_cast<uint8_t>((crc >> 8) & 0xFF));

    return frame;
}

std::vector<uint8_t> FredorchF21sController::buildStepBlock(
    uint8_t speedOut, uint8_t speedIn,
    uint16_t posIn, uint16_t posOut,
    uint16_t repeat,
    uint16_t startReg)
{
    // Modbus RTU Write Multiple Registers (fn 0x10):
    //   01 10 [regHi] [regLo] 00 05 0A [spdOutHi] [spdOutLo] [spdInHi] [spdInLo]
    //   [posInHi] [posInLo] [posOutHi] [posOutLo] [repHi] [repLo]
    //   [CRCLo] [CRCHi]  (19 bytes)
    std::vector<uint8_t> frame;
    frame.reserve(19);
    frame.push_back(0x01);
    frame.push_back(0x10);
    frame.push_back(static_cast<uint8_t>((startReg >> 8) & 0xFF));
    frame.push_back(static_cast<uint8_t>(startReg & 0xFF));
    frame.push_back(0x00); frame.push_back(0x05);  // quantity = 5
    frame.push_back(0x0A);                        // byte count = 10
    // 5 registers x 2 bytes:
    frame.push_back(0x00); frame.push_back(speedOut);
    frame.push_back(0x00); frame.push_back(speedIn);
    frame.push_back(static_cast<uint8_t>((posIn >> 8) & 0xFF));
    frame.push_back(static_cast<uint8_t>(posIn & 0xFF));
    frame.push_back(static_cast<uint8_t>((posOut >> 8) & 0xFF));
    frame.push_back(static_cast<uint8_t>(posOut & 0xFF));
    frame.push_back(static_cast<uint8_t>((repeat >> 8) & 0xFF));
    frame.push_back(static_cast<uint8_t>(repeat & 0xFF));

    const uint16_t crc = modbusCrc16(frame.data(), frame.size());
    frame.push_back(static_cast<uint8_t>(crc & 0xFF));
    frame.push_back(static_cast<uint8_t>((crc >> 8) & 0xFF));

    return frame;
}


// ---------------------------------------------------------------------------
// buildBatchStepTableFrame — builds the sequence of fn 0x10 (Write Multiple
// Registers) frames that program a multi-step "spline" into the F21S
// program-memory slots starting at 0x006B:
//
//   slot i lives at register 0x006B + i * STEP_SLOT_STRIDE and holds:
//     [speedOut] [speedIn] [posIn] [posOut] [repeat]
//
// Firmware execution model (stpihkal #131 + HW tests):
//   - the program runs slot by slot in address order;
//   - a slot with repeat == 0 is the STOP MARKER: the firmware halts the
//     program at that slot without executing it;
//   - a slot with repeat != 0 executes, then the firmware advances to the
//     next slot (a repeat != 0 in a trailing garbage slot is what caused
//     the old "7 wild repeats" bug).
//
// Batch mapping:
//   N steps -> N slots, each carrying all four parameters in register
//             write order: [speed1->speedOut] [speed2->speedIn]
//             [pos1->posIn] [pos2->posOut], repeat = 1.
//             A zero pos or speed is a valid value (speed 0 = slowest,
//             pos 0 = full retract).
//   + ONE all-zero terminator slot (speed 0, pos 0, repeat = 0) so the
//     firmware stops cleanly after the last step instead of reading
//     garbage registers (gated by STEP_TERMINATOR_ON, currently false).
//
// IMPORTANT: speed = 0 is the SLOWEST move (not zero/none), and pos = 0
// means full retract. The terminator works because the firmware stops
// at repeat == 0 without executing the slot.
//
// Play (0x69) and Loop (0x6A) are intentionally NOT part of these frames;
// the worker sends them as separate fn 0x06 frames.
// ---------------------------------------------------------------------------
std::vector<std::vector<uint8_t>> FredorchF21sController::buildBatchStepTableFrame(
    const std::vector<FredorchMoveSteps>& steps, int slotOffset)
{
    std::vector<std::vector<uint8_t>> frames;
    frames.reserve(steps.size() + 1);

    const int n = static_cast<int>(steps.size());
    if (n == 0)
    {
        // Empty batch: single no-op slot (speed 0, pos 0, repeat 1).
        frames.push_back(buildStepBlock(0, 0, 0, 0, 1));
        return frames;
    }

    for (int i = 0; i < n; ++i)
    {
        // Map the user parameters to the register write order:
        //   [speed1->speedOut] [speed2->speedIn] [pos1->posIn] [pos2->posOut]
        // All four values are sent as-is (zero is valid).
        const int posIn    = quantizePosition(steps[i].pos1);
        const int speedOut = clampSpeed(steps[i].speed1);
        const int posOut   = quantizePosition(steps[i].pos2);
        const int speedIn  = clampSpeed(steps[i].speed2);
        const uint16_t startReg =
            static_cast<uint16_t>(REG_STEP_BLOCK + (slotOffset + i) * STEP_SLOT_STRIDE);
        frames.push_back(buildStepBlock(static_cast<uint8_t>(speedOut),
                                        static_cast<uint8_t>(speedIn),
                                        static_cast<uint16_t>(posIn),
                                        static_cast<uint16_t>(posOut),
                                        1,
                                        startReg));
    }

    if (n >= 2 && STEP_TERMINATOR_ON)
    {
        // Stop marker: all zeros, repeat = 0. The firmware halts the
        // program at the first slot with repeat == 0, so the motor stops
        // after the last step without any extra motion.
        const uint16_t termReg =
            static_cast<uint16_t>(REG_STEP_BLOCK + (slotOffset + n) * STEP_SLOT_STRIDE);
        frames.push_back(buildStepBlock(0, 0, 0, 0, 0, termReg));
    }

    return frames;
}

// ---------------------------------------------------------------------------
// Quantization helpers
// ---------------------------------------------------------------------------

int FredorchF21sController::quantizePosition(int pos)
{
    // Сhecked: only 0->150 have different locations,
    // also pos really quantized by 10
    // Clamp to [0, 150].
    if (pos < 0)   pos = 0;
    if (pos > 150) pos = 150;
    // Snap to the nearest grid value (0, 10, 20, ..., 150).
    const int q = (pos + 5) / 10 * 10;  // round to nearest 10
    return std::min(q, 150);
}

int FredorchF21sController::clampSpeed(int speed)
{
    // Checked: only 0->14 have different speed results
    // speed == 0 is not zero it's slowest move
    if (speed < 0)  speed = 0;
    if (speed > 14) speed = 14;
    return speed;
}
