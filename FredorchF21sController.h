// FredorchF21sController.h
//
// Isolated controller for the Fredorch F21S / T8 positional belt actuator,
// driven in Program Mode over BLE via the WinRtBleManager WinRT transport.
//
// Protocol reference: github.com/buttplugio/stpihkal/issues/131
//   GATT layout (base 0000xxxx-0000-1000-8000-00805f9b34fb):
//     Service  : 0xFFB0
//     TX char  : 0xFFB1 (Write / WriteWithoutResponse) — Modbus RTU frames
//     Rx char  : 0xFFB2 (Notify) — telemetry / ACKs
//   Frame = Modbus RTU, unit id 0x01, CRC-16/MODBUS (poly 0xA001, init
//   0xFFFF), CRC appended little-endian (Lo, Hi).
//
// Program Mode registers:
//   0x0064 = mode select: 0=pattern, 1=program, 2=free
//   0x0069 = play/record: 1=play, 0=record/stop
//   0x006A = loop:        1=loop on, 0=loop off
//   0x006B = step block start (5 registers via fn 0x10, or packed single
//            register via fn 0x06)
//
// One-step "trick": write one step (position + speed) then 0x0069=1 to play;
// with loop off the actuator runs exactly that step and stops on its own.
//
// Threading model:
//   - A dedicated worker thread owns all frame emission (so WinRT GATT
//     writes happen off the UI thread, and the ~90ms throttle is enforced
//     by wall-clock time rather than by waiting for GATT Completed
//     delegates).
//   - moveTo() / stop() are thread-safe and non-blocking: they update a
//     last-command-wins CommandSlot (generation counter + pendingStop +
//     pendingMove) under a single mutex and wake the worker.
//   - stop() is high priority: it increments the generation (invalidating
//     any in-flight two-stage moveTo), erases pendingMove, and sets
//     pendingStop. The worker, between the two frames of a moveTo, re-checks
//     the generation so a mid-flight stop() suppresses the play frame and
//     emits a stop frame instead. No self-restart is possible.
//
// All code, logs, comments are in ENGLISH per project spec
// (.roo/rules/agents.md).

#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>
#include <optional>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>
#include <functional>

#include <QString>

#include "WinRtBleManager.h"

class FredorchF21sController
{
public:
    // -----------------------------------------------------------------
    // Move-step descriptor (batch / spline API)
    // -----------------------------------------------------------------
    // One program slot with ALL FOUR movement parameters, named after the
    // F21S register write order (stpihkal #131):
    //   [speed1 -> speedOut] [speed2 -> speedIn]
    //   [pos1   -> posIn]    [pos2   -> posOut]
    // (the 5th register, repeat count, is always 1 for a batch slot).
    //
    // All four fields are always sent to the registers as-is (zero is a
    // valid value, it is never replaced by another field).
    //
    // Positions are in 0..150 (auto-quantized to the 0,10,...,150 grid),
    // speeds are clamped to 0..14 (speed 0 = slowest move, NOT zero speed).
    struct FredorchMoveSteps
    {
        int pos1   = 0;   // -> positionIn  register
        int speed1 = 0;   // -> speedOut    register
        int pos2   = 0;   // -> positionOut register
        int speed2 = 0;   // -> speedIn     register
    };

    // -----------------------------------------------------------------
    // Configuration
    // -----------------------------------------------------------------
    struct Config
    {
        uint64_t deviceAddress = 0;       // BLE MAC address (for connect)
        std::string deviceName = "F21S";  // human-readable name (diagnostics)
        int throttleMs = 90;              // min gap between move commands (ms)
        // true  = 5-register step block (fn 0x10, 19 bytes) — the only step
        //         encoding documented in stpihkal #131; default.
        // false = packed single-register step (fn 0x06, 8 bytes) — undocumented,
        //         NOT recommended (only register 0x6B is written, so the
        //         position registers 0x6D/0x6E are left stale).
        bool useStepBlock = true;
        // Timeout (ms) for the blocking per-frame GATT completion wait.
        // Every frame is sent via WriteWithoutResponse and we wait for the
        // completion delegate, so a following frame (step -> play) is never
        // canceled mid-flight by LATEST-WINS coalescing.
        int frameTimeoutMs = 300;
        int connectTimeoutMs = 8000;      // GATT handshake budget (ms)
        // Motor-settle window (ms) inserted after the Play OFF frame before
        // the batch 0x10 write lands. Gives the MCU firmware time to fully
        // halt the rocker arm and unlock the program-memory window.
        // Empirically 50 ms is required on the F21S (YXLinksSPP bridge)
        // for back-to-back moveTo() calls to both execute reliably.
        int stopSettleMs = 50;
    };

    explicit FredorchF21sController(WinRtBleManager* bleManager,
                                    Config config = Config{});
    ~FredorchF21sController();

    // Non-copyable, non-movable (owns a worker thread).
    FredorchF21sController(const FredorchF21sController&) = delete;
    FredorchF21sController& operator=(const FredorchF21sController&) = delete;
    FredorchF21sController(FredorchF21sController&&) = delete;
    FredorchF21sController& operator=(FredorchF21sController&&) = delete;

    // -----------------------------------------------------------------
    // Lifecycle
    // -----------------------------------------------------------------
    // Connect: GATT handshake (FFB0 -> FFB1) + Program Mode init sequence
    //   (0x0064=1, 0x006A=0, 0x0069=0). Starts the worker thread.
    //   Uses the deviceAddress from the constructor Config.
    //   Returns true on success.
    bool connect();

    // Connect to a specific BLE MAC address (overrides the Config address
    // for this session only). Same behavior as connect() but lets the
    // caller address the device at call time.
    bool connectWithAddress(uint64_t deviceAddress);

    // Disconnect: send a stop frame, flush the worker, tear down the GATT
    //   link (releasing the radio for Intiface Central).
    void disconnect();

    bool isConnected() const { return m_connected.load(std::memory_order_acquire); }

    // Diagnostic detail from the last failed operation (thread-safe).
    QString lastError() const;

    // -----------------------------------------------------------------
    // Control API (thread-safe, non-blocking)
    // -----------------------------------------------------------------
    // Permute the actuator to targetPos at the given speed (single-step form).
    //   targetPos: 0..150 (quantized to the 16-value grid 0,10,...,150)
    //   speed:     0..14  (clamped; 0 = slowest)
    //   Delegates to moveTo({{targetPos, speed, targetPos, speed}},
    //   /*slotOffset=*/0, /*loop=*/false).
    //   Latest-wins: if called faster than the throttle, only the freshest
    //   command is sent. Returns true if the command was queued.
    bool moveTo(int targetPos, int speed);

    // Permute the actuator along a sequence of FredorchMoveSteps slots —
    // the "spline" / batch form. Slot i of the batch is written to program
    // slot (slotOffset + i) at register 0x006B + (slotOffset + i) * 6 with
    // [speed1->speedOut] [speed2->speedIn] [pos1->posIn] [pos2->posOut].
    //
    // Full (re)start sequence (any call when the device is NOT looping):
    //   1. Play OFF (0x06, 0x69=0) + stopSettleMs — unlocks the step block.
    //   2. Loop   (0x06, 0x6A = loop?1:0).
    //   3. N 0x10 (Write Multiple Registers) frames programming the slots:
    //      one slot per step (pos1/speed1/pos2/speed2 mapped to the
    //      posIn/speedOut/posOut/speedIn registers, repeat = 1).
    //   4. Play ON (0x06, 0x69=1).
    //
    // PING-PONG live rewrite: if the device is currently playing a LOOPING
    // program (previous call used loop=true), the worker skips Play OFF /
    // Loop / Play ON and rewrites only the slots from slotOffset onward —
    // the firmware picks up the new data on the next lap with zero dead
    // time. This allows chaining 10-step splines back-to-back:
    //   moveTo(0, first10Steps,  true)  // start looping
    //   ...
    //   moveTo(0, next2Steps,    true)  // live-rewrites slots 0..1
    //
    // NOTE: Play/Loop must be separate 0x06 frames (verified against the
    // buttplug reference implementation, fredorch.rs).
    //   steps:      any number >= 1; slotOffset + steps.size() <= 10.
    //   slotOffset: 0..9 — first slot index to program (0 = full (re)start).
    //   loop:       if true, the sequence replays continuously until stop().
    //               Ignored during a live rewrite (loop state untouched).
    //   Latest-wins: a newer call overwrites any pending batch.
    bool moveTo(const std::vector<FredorchMoveSteps>& steps, bool loop = false);
    bool moveTo(const std::vector<FredorchMoveSteps>& steps,
                int slotOffset, bool loop);

    // Stop motion (high priority, bypasses throttling).
    //   Emits a zero step + 0x0069=0, then 0x0064=0 to return control to
    //   the RF remote (Pattern mode). Invalidates any pending / in-flight
    //   moveTo. Returns true if the stop was queued.
    bool stop();

    // Start looping between two positions at the given speed (Intiface-style).
    //   posA: start position 0..150 (quantized)
    //   posB: end position 0..150 (quantized)
    //   speed: 0..14 (clamped; 0 = slowest)
    // Sends: Loop ON (0x6A=1), step block (posA->posB), Play (0x69=1).
    // The device then continuously oscillates A<->B.
    // Thread-safe, non-blocking (queued on the worker thread).
    bool startLoopRange(int posA, int posB, int speed);

    // Explicitly re-enter Program Mode (single frame: 0x0064 = 1).
    // stop() and disconnect() return the device to Pattern mode
    // (0x0064 = 0, RF-remote control), so call this before the next
    // moveTo() batch when the device is in Pattern mode:
    //   startProgramMode();
    //   moveTo(steps, /*loop=*/true);
    //   moveTo(nextSteps, /*loop=*/true);   // live rewrite
    //   stop();
    // Thread-safe, non-blocking (queued on the worker thread).
    bool startProgramMode();

    // Raw frame send (for tests / advanced use). Bypasses the worker and
    // throttle; dispatches directly to the BLE transport.
    bool sendFrame(const std::vector<uint8_t>& frame);

    // -----------------------------------------------------------------
    // Read the Play/Stop state register (0x0069) — Modbus fn 0x03.
    //
    // Request : 01 03 00 69 00 01 [CRC_LO] [CRC_HI]  (8 bytes)
    // Response (via FFB2 notify): 01 03 02 [ValHi] [ValLo] [CRC_LO] [CRC_HI]
    //   ValHi == 0x00 && ValLo == 0x01  -> Play ON (motor is playing)
    //   ValHi == 0x00 && ValLo == 0x00  -> Play OFF (motor stopped)
    //
    // This is a BLOCKING call: it sends the read request, then waits up to
    // `timeoutMs` for the 7-byte Modbus response to arrive via the FFB2
    // notify callback. Returns true if a valid response was received.
    // `stateOut` receives the decoded register value (1 = Play, 0 = Stop).
    // Thread-safe. Also used from the worker thread as a BLE keep-alive
    // while a looping program plays (blocks for at most `timeoutMs`).
    //
    // Intended use: manual verification / test that the YXLinksSPP BLE
    // bridge is out of power-save and the link is alive.
    // -----------------------------------------------------------------
    bool readPlayState(uint16_t* stateOut, int timeoutMs = 500);

    // -----------------------------------------------------------------
    // Static Modbus helpers
    // -----------------------------------------------------------------
    // CRC-16/MODBUS (poly 0xA001, init 0xFFFF, reflected).
    static uint16_t modbusCrc16(const uint8_t* data, size_t len);

    // Build a Modbus RTU "Write Single Register" (fn 0x06) frame:
    //   01 06 [RegHi] [RegLo] [ValHi] [ValLo] [CRCLo] [CRCHi]  (8 bytes)
    static std::vector<uint8_t> buildWriteSingleRegister(uint16_t regAddr,
                                                         uint16_t value);

    // Build a Modbus RTU "Write Multiple Registers" (fn 0x10) step block:
    //   01 10 [regHi] [regLo] 00 05 0A [speedOut] [speedIn] [posIn] [posOut]
    //   [repeat] [CRCLo] [CRCHi]  (19 bytes)
    // startReg defaults to 0x006B (the first program slot); multi-step
    // batches address slot i at 0x006B + i * 6.
    static std::vector<uint8_t> buildStepBlock(uint8_t speedOut,
                                               uint8_t speedIn,
                                               uint16_t posIn,
                                               uint16_t posOut,
                                               uint16_t repeat,
                                               uint16_t startReg = 0x006B);

    // Build the sequence of fn 0x10 (Write Multiple Registers) frames that
    // program a multi-step spline into the F21S program-memory slots:
    //   step i is written to program slot (slotOffset + i) at register
    //   0x006B + (slotOffset + i) * 6.
    // Each frame carries all four slot parameters in register write order:
    //   [speed1->speedOut] [speed2->speedIn] [pos1->posIn] [pos2->posOut]
    // (repeat = 1 for all batch slots).
    // Play/Loop are NOT part of these frames; the worker sends them as
    // separate 0x06 frames (or skips them in live-rewrite mode).
    static std::vector<std::vector<uint8_t>> buildBatchStepTableFrame(
        const std::vector<FredorchMoveSteps>& steps, int slotOffset = 0);

    // -----------------------------------------------------------------
    // Quantization helpers (public for testing)
    // -----------------------------------------------------------------
    // Snap a position to the 16-value grid (0, 10, 20, ..., 150), clamped
    // to [0, 150].
    static int quantizePosition(int pos);

    // Clamp a speed to [0, 14] (0 = slowest move, NOT zero speed).
    static int clampSpeed(int speed);

private:
    // Thread-safe command slot (last-command-wins). Guarded by m_mtx.
    // pendingSteps replaces the old single-step pendingMove: the whole
    // batch (1..14 quantized steps) is stored here along with the loop flag.
    struct CommandSlot
    {
        std::mutex mtx;
        std::condition_variable cv;
        uint64_t generation = 0;                  // bumped by moveTo() and stop()
        bool pendingStop = false;                 // high-priority stop request
        bool pendingInit = false;                 // send the Program Mode init sequence
        bool pendingProgramMode = false;          // explicit 0x0064=1 (re-enter Program Mode)
        std::optional<std::vector<FredorchMoveSteps>> pendingSteps;  // latest-wins batch
        bool pendingLoop = false;                 // loop flag for pendingSteps
        int pendingSlotOffset = 0;                // first slot index for pendingSteps
        bool pendingLoopRange = false;            // startLoopRange is queued
        int loopPosA = 0;                         // start position for loop range
        int loopPosB = 0;                         // end position for loop range
        int loopSpeed = 5;                        // speed for loop range
    };

    WinRtBleManager* m_bleManager = nullptr;
    Config m_config;
    // FFB2 notify response capture (used by readPlayState()).
    // The raw-notify callback registered in connect() stores every payload
    // in m_lastNotifyBytes and bumps m_notifySeq. readPlayState() then
    // polls for a new valid Modbus read reply.
    std::vector<uint8_t> m_lastNotifyBytes;   // guarded by m_playStateMtx
    std::atomic<uint64_t> m_notifySeq { 0 };

    std::atomic<bool> m_connected { false };
    std::atomic<bool> m_shutdown { false };

    // Play/loop state tracking (worker thread only) — drives the
    // ping-pong live-rewrite optimization: when both are true, a new
    // moveTo() rewrites slots on the fly without Play OFF / Play ON.
    std::atomic<bool> m_playing { false };
    std::atomic<bool> m_lastLoopOn { false };
    // Wall-clock time of the last frame that left the radio (worker-thread
    // only, no locking). Drives the BLE keep-alive: while a looping program
    // plays the device emits no traffic, and the BLE bridge drops into
    // power-save after ~1s of silence.
    std::chrono::steady_clock::time_point m_lastTx {};

    mutable std::mutex m_lastErrorMtx;
    std::string m_lastError;

    mutable std::mutex m_playStateMtx;

    CommandSlot m_slot;
    std::thread m_workerThread;
    std::mutex m_threadMtx;   // guards start/stop of m_workerThread

    void startWorker();
    void stopWorker();
    void workerLoop();

    void sendInitSequence();
    std::vector<uint8_t> buildStopFrame() const;
    std::vector<uint8_t> buildPlayFrame() const;
    std::vector<uint8_t> buildStepFrame(int pos, int speed) const;

    bool sendFrameInternal(const std::vector<uint8_t>& frame);
    static std::string describeFrame(const std::vector<uint8_t>& frame);
    void logFrame(const std::vector<uint8_t>& frame) const;
    void setLastError(const std::string& s);
};
