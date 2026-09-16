// ThreadAffinity.h
//
// Runtime thread-affinity helper for HismithControl.
//
// Purpose
// -------
// On hybrid CPUs (Intel 12th-14th Gen, AMD Strix, Arrow Lake, Meteor
// Lake, ARM big.LITTLE) the Windows scheduler is free to migrate any
// thread between P-cores (high-perf) and E-cores (efficiency). For
// low-latency work (WinRT BLE write-completed delegates, OpenCV
// capture, HTTP polling of the video player) that migration produces
// the "50 fps randomly, 60 fps randomly" pattern the user sees with
// "Direct Bluetooth LE (WinRT)" vs. the stable ~60 fps of
// "Intiface Central (Buttplug.io)" — Buttplug is a plain socket and
// does not care which core it runs on, but WinRT's Completed
// delegates DO (they ride on the Windows message queue of the UI
// thread and are sensitive to core-migration jitter).
//
// We pin each latency-sensitive thread to exactly ONE logical CPU.
//
// ---------------------------------------------------------------------
// Which core do we pin to? — LEVEL-C discovery (NEW, Win10 21H1+)
// ---------------------------------------------------------------------
// Historically the code assumed "indices 0..N-1 are P-cores" (the
// "P-first heuristic"). That holds empirically on most Intel hybrid
// parts, but is NOT a Win32 API contract — on Arrow Lake (your 285K:
// 8 P + 16 E on one die, no SMT) and on AMD Strix the actual order is
// decided by the platform's BIOS / ACPI tables, and the same CPU model
// can come out in different order on different motherboards.
//
// The fix: ask the OS directly. Windows 10 21H1+ exposes
// SYSTEM_CPU_SET_INFORMATION via GetSystemCpuSetInformation(). Each
// CPU set carries an `EfficiencyClass` field. Per MSDN:
//
//     "CPU Sets with higher numerical values of this field have home
//      processors that are faster but less power-efficient than ones
//      with lower values."
//
// i.e. the HIGHEST EfficiencyClass present in a system is the P-class.
// This is the same signal Windows Task Manager uses to colour the
// per-core tiles. We now:
//
//   1. enumerate every CPU set once (at first pin call — lazy),
//   2. find the max EfficiencyClass,
//   3. collect every logical index whose class == max,
//   4. sort + cache in a C++17 inline namespace variable (a single
//      shared copy across all translation units, per process),
//   5. map "slot 0..N-1" → the i-th entry of that sorted list,
//   6. pin with SetThreadAffinityMask(1ULL << real_index).
//
// On your 285K this should resolve to 8 real P-logical-indices —
// whether or not they happen to be 0..7 depends on the BIOS. The log
// line in execution.log will SHOW you the answer with both the slot
// number (what we asked for) AND the resolved real index (what
// actually got pinned). That replaces "trust us" with "here's the
// proof".
//
// Globals (defined in main.cpp, declared here)
// --------------------------------------------
//   g_align_to_p_cores  : master switch. When false, every pin*() call
//                         is a no-op and threads stay where the OS
//                         scheduler put them (default Windows
//                         behaviour).
//   g_p_core_count      : count of PIN SLOTS to reserve. This is NOW a
//                         pin-target count, NOT a "P-core count":
//                         -1  = disable pinning (same as
//                               g_align_to_p_cores=false)
//                          0  = auto: min(4, real_P_core_count)
//                               where real_P_core_count is the # of
//                               logical CPUs the OS reports at the
//                               top EfficiencyClass (falls back to
//                               hardware_concurrency() on non-hybrid
//                               / pre-Win10 21H1)
//                         >0  = exactly N slots (real_P_core_indices[0..N-1])
//
// Thread slot allocation (contract with call-sites)
// -------------------------------------------------
//   slot 0 : UI thread (main thread running QApplication + a.exec()).
//            WinRT Completed delegates fire through PostMessage into
//            THIS thread's message queue, so the UI thread MUST live
//            on a P-core for stable ~60 fps of BLE speed writes.
//   slot 1 : main worker (run_funscript / test_hismith /
//            get_performance_with_hismith / get_statistics_with_hismith).
//            All four are mutually exclusive via the g_work_in_progress
//            flag and never run concurrently, so they share this one
//            P-core slot.
//   slot 2 : ThreadedCapture::capture_loop — OpenCV frame pump.
//   slot 3 : ThreadedPlayerStatus::status_loop — HTTP poll of the
//            video player (QNetworkAccessManager).
//
// Call sites (T3 of the project plan, unchanged)
// ----------------------------------------------
//   1. main.cpp   — main() after log-file setup              -> slot 0
//   2. main.cpp   — ThreadedCapture::capture_loop            -> slot 2
//   3. main.cpp   — ThreadedPlayerStatus::status_loop        -> slot 3
//   4. mainwindow.cpp — StartWorker::doWork                  -> slot 1
//   5. mainwindow.cpp — GetStatisticsWorker::doWork          -> slot 1
//   6. mainwindow.cpp — TestWorker::doWork                   -> slot 1
//   7. mainwindow.cpp — PerfWorker::doWork                   -> slot 1
//
// Semantics
// ---------
//  * `pinCurrentThread(slot, tag)` is idempotent — calling it multiple
//    times on the same thread is safe (SetThreadAffinityMask /
//    SetThreadPriority are both re-entrant and side-effect-free on
//    repeat calls).
//  * Toggling `g_align_to_p_cores` at runtime ONLY affects threads that
//    are started AFTER the toggle (already-pinned threads keep their
//    mask).
//  * The function returns true only if SetThreadAffinityMask succeeded.
//    A diagnostic line (with the tag, the slot, the RESOLVED real
//    logical index, the success of each Win32 call, and the
//    real_P_map flag) is always emitted via qDebug() -> captured both
//    on the VS debugger output AND in <exe_directory>\execution.log by
//    the global Qt message handler in main.cpp. That is the only way to
//    prove, after the fact, which logical processor each thread is
//    actually on.
//
// Compatibility
// -------------
//  * GetSystemCpuSetInformation requires Windows 10 21H1+ (build
//    19041+) and Windows SDK 10.0.19041+. On older platforms
//    pCoreIndices() returns an empty vector and we fall back to the
//    legacy P-first heuristic — same behaviour as before this change.
//  * SetThreadAffinityMask is per-group (single logical mask of 64 bits)
//    — a CPU with more than 64 logical processors in one group is
//    effectively unreachable by this code path. We log a warning for
//    that case but still return false gracefully.

#ifndef THREAD_AFFINITY_H
#define THREAD_AFFINITY_H

#include <windows.h>
#include <process.h>             // GetThreadPriority, SetThreadPriority
#include <processthreadsapi.h>   // GetSystemCpuSetInformation, SYSTEM_CPU_SET_INFORMATION
#include <thread>
#include <atomic>
#include <algorithm>
#include <mutex>
#include <vector>
#include <QString>
#include <QStringList>
#include <QDebug>      // qDebug/qInfo -> routed into <exe>\execution.log by the global Qt msg handler in main.cpp
#include <cstdlib>     // for std::stoul fallback

namespace ThreadAffinity
{
    // Globals (defined in main.cpp): master switch + pin-slot count.
    extern std::atomic<bool> g_align_to_p_cores;
    extern std::atomic<int>  g_p_core_count;

    // Tag strings (defined in main.cpp) so every pin call-site can log
    // WHICH logical thread just got pinned. Kept as bare pointers — these
    // live in .rdata and are never mutated after startup.
    extern const char* const kTagUiWorkerMain;
    extern const char* const kTagWorkerMainLoop;
    extern const char* const kTagThreadedCapture;
    extern const char* const kTagThreadedPlayer;

    // -----------------------------------------------------------------
    // Level-C P-core discovery (Win10 21H1+)
    // -----------------------------------------------------------------
    // C++17 inline namespace-scoped variables — a single shared instance
    // per process across every translation unit that includes this
    // header. `std::call_once` gives us thread-safe lazy init.
    inline std::once_flag   s_pCoreOnce{};
    inline std::vector<int> s_pCoreList{};

    // Lazily-built, sorted list of REAL logical processor indices the OS
    // (Intel Thread Director + Win11) identifies as P-cores. Backed by
    // the `EfficiencyClass` field of SYSTEM_CPU_SET_INFORMATION via
    // GetSystemCpuSetInformation.
    //
    // - on a hybrid CPU, the HIGHEST EfficiencyClass present wins, and
    //   we return every logical index carrying that class;
    // - on a non-hybrid CPU the OS typically reports a single class for
    //   every logical CPU — in which case the vector == 0..(hc-1),
    //   which is the correct "all cores are equally P" answer;
    // - empty == OS did not expose P/E information (pre-Win10 21H1, or
    //   the call itself failed), and callers should fall back to the
    //   legacy P-first heuristic.
    inline const std::vector<int>& pCoreIndices()
    {
        std::call_once(s_pCoreOnce, [] {
            ULONG need = 0;
            if (!GetSystemCpuSetInformation(nullptr, 0, &need, nullptr, 0) && need == 0)
                return;             // API not available / no CPU sets reported
            if (need == 0) return;

            std::vector<BYTE> buf(need);
            ULONG got = 0;
            if (!GetSystemCpuSetInformation(
                    reinterpret_cast<PSYSTEM_CPU_SET_INFORMATION>(buf.data()),
                    need, &got, nullptr, 0) || got == 0)
                return;

            const BYTE* p   = buf.data();
            const BYTE* end = buf.data() + got;
            int maxEClass   = -1;

            // Pass 1: determine the highest EfficiencyClass in the system
            while (p + sizeof(SYSTEM_CPU_SET_INFORMATION) <= end)
            {
                const auto* info = reinterpret_cast<const SYSTEM_CPU_SET_INFORMATION*>(p);
                if (info->Size == 0 || info->Type != 0u /* CPU_SET */) break;
                const int ec = info->CpuSet.EfficiencyClass;
                if (ec > maxEClass) maxEClass = ec;
                p += info->Size;
            }
            if (maxEClass < 0) return;

            // Pass 2: collect every index that carries that max class
            const BYTE* q = buf.data();
            while (q + sizeof(SYSTEM_CPU_SET_INFORMATION) <= end)
            {
                const auto* info = reinterpret_cast<const SYSTEM_CPU_SET_INFORMATION*>(q);
                if (info->Size == 0 || info->Type != 0u /* CPU_SET */) break;
                if (info->CpuSet.EfficiencyClass == static_cast<BYTE>(maxEClass))
                    s_pCoreList.push_back(static_cast<int>(info->CpuSet.LogicalProcessorIndex));
                q += info->Size;
            }
            std::sort(s_pCoreList.begin(), s_pCoreList.end());
        });
        return s_pCoreList;
    }

    // Map a 0-based pin slot into a real logical processor index.
    // Returns:
    //   >= 0  — the real logical index to pin to.
    //    -1   — slot out of range for the real P list.
    //
    // Fallback behaviour when no P/E info is exposed (pre-Win10 21H1):
    // the slot is treated as a raw logical index (legacy P-first).
    inline int pCoreIndexForSlot(int slot)
    {
        const auto& P = pCoreIndices();
        if (!P.empty())
        {
            if (slot < 0) return -1;
            if (slot >= static_cast<int>(P.size())) return -1;
            return P[slot];
        }
        return slot;
    }

    // -----------------------------------------------------------------
    // Resolve the final count of pin-eligible slots (or 0 when disabled).
    // -----------------------------------------------------------------
    // The HARD CEILING is the real # of P-cores reported by the OS when
    // available (so we can never try to pin to a non-existent P-core).
    // On non-hybrid systems the OS reports every CPU with one class —
    // the ceiling becomes hardware_concurrency(), matching the legacy
    // behaviour exactly.
    inline int resolvedPCoreCount()
    {
        if (!g_align_to_p_cores.load(std::memory_order_relaxed)) return 0;
        const int n = g_p_core_count.load(std::memory_order_relaxed);
        if (n < 0) return 0;     // explicit "no pin"
        const int hc = int(std::thread::hardware_concurrency());
        if (hc <= 0) return 0;

        const auto& P = pCoreIndices();
        const int hardCap = (!P.empty()) ? static_cast<int>(P.size()) : hc;

        if (n == 0)
        {
            // Auto mode: default = 4 pin targets. 4 matches the current
            // number of latency-critical call-sites in this project (UI,
            // worker, capture, playerStatus — see the slot table at the
            // top of this file). Raising kAutoPinDefault only makes sense
            // after the 5th thread is introduced; until then it is
            // inert (nothing would use slots >= 4).
            constexpr int kAutoPinDefault = 4;
            return (kAutoPinDefault < hardCap) ? kAutoPinDefault : hardCap;
        }
        return (n < hardCap) ? n : hardCap;
    }

    // -----------------------------------------------------------------
    // Pin the current thread to the `slot`-th P-core. The slot is
    // 0-based into the REAL P-core list reported by the OS (see
    // pCoreIndices()); the actual logical processor index used is that
    // P-core's LogicalProcessorIndex from SYSTEM_CPU_SET_INFORMATION.
    //
    // No-op (returns false) when:
    //   - g_align_to_p_cores == false, or g_p_core_count < 0
    //   - slot < 0 or slot >= resolvedPCoreCount()
    //   - the resolved logical index falls outside the single-group
    //     affinity range 0..63 (rare: needs > 64 logical CPUs)
    //
    // On success:
    //   - SetThreadAffinityMask(GetCurrentThread(), 1ULL << real_idx)
    //   - SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL)
    //
    // A diagnostic line (always emitted; lands in
    // <exe_directory>\execution.log via qDebug) contains:
    //   tag, slot, real_logical_idx, real_p_map(yes|no),
    //   SetThreadAffinityMask=OK|FAILED,
    //   SetThreadPriority    =OK|FAILED,
    //   mask=0x..., priority=N
    //
    // This is the single source of truth at runtime for "what
    // logical CPU am I actually on".
    // -----------------------------------------------------------------
    inline bool pinCurrentThread(int slot, const char* tag = nullptr)
    {
        const int  N  = resolvedPCoreCount();
        const char* t = (tag != nullptr) ? tag : "(null)";

        if (N <= 0)
        {
            qDebug().noquote()
                << QString("[ThreadAffinity] tag=\"%1\" SKIPPED (g_align_to_p_cores = false or g_p_core_count < 0)")
                       .arg(t);
            return false;
        }

        if (slot < 0 || slot >= N)
        {
            qDebug().noquote()
                << QString("[ThreadAffinity] tag=\"%1\" SKIPPED (slot=%2 out of range 0..%3)")
                       .arg(t).arg(slot).arg(N - 1);
            return false;
        }

        const int realIdx = pCoreIndexForSlot(slot);
        if (realIdx < 0 || realIdx >= 64)
        {
            qWarning().noquote()
                << QString("[ThreadAffinity] tag=\"%1\" FAILED (resolved logical idx=%2 "
                           "out of single-group affinity range 0..63; this CPU requires "
                           "SetThreadGroupAffinity which is not implemented)")
                       .arg(t).arg(realIdx);
            return false;
        }

        HANDLE h         = GetCurrentThread();
        const ULONGLONG mask = (1ULL << realIdx);

        const BOOL okMask = SetThreadAffinityMask(h, mask);
        const BOOL okPrio = SetThreadPriority(h, THREAD_PRIORITY_TIME_CRITICAL);

        const int curPrio      = GetThreadPriority(h);
        const bool usingRealMap = !pCoreIndices().empty();

        qDebug().noquote()
            << QString("[ThreadAffinity] tag=\"%1\" slot=%2 real_logical_idx=%3 real_p_map=%4 "
                       "SetThreadAffinityMask=%5 SetThreadPriority=%6 "
                       "mask=%7 priority=%8")
                    .arg(t)
                    .arg(slot)
                    .arg(realIdx)
                    .arg(usingRealMap ? "yes" : "no (P-first fallback)")
                    .arg(okMask ? "OK" : "FAILED")
                    .arg(okPrio ? "OK" : "FAILED")
                    .arg(mask)
                    .arg(curPrio);

        return (okMask != 0);
    }

    // -----------------------------------------------------------------
    // One-shot topology dump. Returns a multi-field string; the caller
    // is expected to emit it via qDebug() so it lands in
    // <exe_directory>\execution.log.
    // -----------------------------------------------------------------
    // The most important new fields:
    //   real_P_core_count     — # of P-cores the OS reports
    //   p_core_indices=[..]  — the REAL logical indices of those P-cores
    //   slot_map=[0->r0 ...] — per-slot resolution (what slot N maps to)
    //   using_real_P_map      — yes (real discovery) / no (legacy heuristic)
    //
    // Run once at startup and grep "p_core_indices" in execution.log:
    // that line is the ground truth for "where are my P-cores" on any
    // given install, no guesswork required.
    inline QString dumpTopology()
    {
        SYSTEM_INFO si{};
        GetSystemInfo(&si);

        const int total = int(std::thread::hardware_concurrency());
        const int N     = resolvedPCoreCount();
        const int gval  = g_p_core_count.load(std::memory_order_relaxed);
        const bool en   = g_align_to_p_cores.load(std::memory_order_relaxed);
        const bool realMap = !pCoreIndices().empty();
        const auto& P = pCoreIndices();

        QString pListStr;
        if (realMap)
        {
            QStringList s;
            for (int i : P) s << QString::number(i);
            pListStr = s.join(",");
        }
        else
        {
            pListStr = "(no P/E info at OS level — fallback to P-first 0..N-1)";
        }

        QString slotMap;
        for (int i = 0; i < N && i >= 0; ++i)
        {
            const int real = pCoreIndexForSlot(i);
            if (!slotMap.isEmpty()) slotMap += ' ';
            slotMap += QString::number(i) + "->" + QString::number(real);
        }

        return QString(
            "[ThreadAffinity] topology: "
            "hardware_concurrency=%1  "
            "g_align_to_p_cores=%2  "
            "g_p_core_count=%3  "
            "resolved_pin_slots=%4  "
            "real_P_core_count=%5  "
            "p_core_indices=[%6]  "
            "slot_map=[%7]  "
            "using_real_P_map=%8  "
            "arch=0x%9  "
            "active_processor_mask=0x%10")
            .arg(total)
            .arg(en ? "true" : "false")
            .arg(gval)
            .arg(N)
            .arg(realMap ? QString::number(P.size()) : "auto")
            .arg(pListStr)
            .arg(slotMap.isEmpty() ? QString("none") : slotMap)
            .arg(realMap ? "yes" : "no")
            .arg((int)si.wProcessorArchitecture)
            .arg((ULONGLONG)si.dwActiveProcessorMask);
    }
}

#endif // THREAD_AFFINITY_H
