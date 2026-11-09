//
// Copyright (c) .NET Foundation and Contributors
// See LICENSE file in the project root for full license information.
//

// GC timing hooks (NANO_POSIX_GC_TIMING). Each record is one "[GCT]" line on the debug output, so it
// is ordered with Debug.WriteLine output. Durations are in nanoseconds. On QNX x86_64 ClockCycles()
// reads the TSC, so a thread moving between CPUs with unsynchronised TSCs can produce a wrapped
// (huge) duration; pin the process to one CPU (runmask) if that shows up.

#include <cinttypes>
#include <cstdio>
#include <ctime>

#include <nanoCLR_Runtime.h>
#include <nanoCLR_GcTiming.h>

#if defined(__QNX__)
#include <sys/neutrino.h>
#include <sys/syspage.h>
#endif

namespace
{
#if defined(__QNX__)
const char *const c_ClockName = "ClockCycles";

uint64_t TicksPerSecond()
{
    return SYSPAGE_ENTRY(qtime)->cycles_per_sec;
}
#else
const char *const c_ClockName = "CLOCK_MONOTONIC";

uint64_t TicksPerSecond()
{
    return 1000000000u;
}
#endif

uint64_t ToNs(uint64_t ticks)
{
    static const uint64_t perSecond = TicksPerSecond();

    if (perSecond == 0)
    {
        return 0;
    }

    // split to avoid overflowing ticks * 1e9
    return ticks / perSecond * 1000000000u + ticks % perSecond * 1000000000u / perSecond;
}

// Writes an snprintf result, dropping it on error and clamping a truncated one.
void EmitFormatted(const char *text, int length, size_t capacity)
{
    if (length > 0)
    {
        CLR_Debug::Emit(text, length < (int)capacity ? length : (int)capacity - 1);
    }
}

void Emit(const char *line, int length, size_t capacity)
{
    static bool headerDone = false;

    if (!headerDone)
    {
        char header[128];
        int n = std::snprintf(
            header,
            sizeof(header),
            "[GCT] clock=%s ticks_per_sec=%" PRIu64 "\r\n",
            c_ClockName,
            TicksPerSecond());
        EmitFormatted(header, n, sizeof(header));
        headerDone = true;
    }

    EmitFormatted(line, length, capacity);
}
} // namespace

uint64_t GcTiming_Now()
{
#if defined(__QNX__)
    return ClockCycles();
#else
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
#endif
}

void GcTiming_ReportCollection(
    uint32_t run,
    const uint64_t (&stamps)[c_GcTiming_Count],
    uint32_t markSlowPasses,
    const GcTimingMarkStack &markStack,
    uint32_t usedBytes,
    uint32_t heapBytes)
{
    auto span = [&](int i) { return ToNs(stamps[i] - stamps[i - 1]); };

    char line[448];
    int n = std::snprintf(
        line,
        sizeof(line),
        "[GCT] gc run=%" PRIu32 " total_ns=%" PRIu64 " prepare_ns=%" PRIu64 " mark_ns=%" PRIu64 " markweak_ns=%" PRIu64
        " sweep_ns=%" PRIu64 " ratio_ns=%" PRIu64 " pressure_ns=%" PRIu64 " markslow_passes=%" PRIu32
        " markstack_chunks=%" PRIu32 " markstack_entries=%" PRIu32 " markstack_failed=%" PRIu32
        " markstack_alloc_ns=%" PRIu64 " used_bytes=%" PRIu32 " heap_bytes=%" PRIu32 "\r\n",
        run,
        ToNs(stamps[c_GcTiming_Count - 1] - stamps[c_GcTiming_Start]),
        span(c_GcTiming_Prepare),
        span(c_GcTiming_Mark),
        span(c_GcTiming_MarkWeak),
        span(c_GcTiming_Sweep),
        span(c_GcTiming_Ratio),
        span(c_GcTiming_Pressure),
        markSlowPasses,
        markStack.chunks,
        markStack.entries,
        markStack.failedAttempts,
        ToNs(markStack.allocTicks),
        usedBytes,
        heapBytes);

    Emit(line, n, sizeof(line));
}

void GcTiming_ReportCompaction(uint32_t run, uint64_t start, uint64_t end, uint32_t usedBytes, uint32_t heapBytes)
{
    char line[160];
    int n = std::snprintf(
        line,
        sizeof(line),
        "[GCT] compact run=%" PRIu32 " total_ns=%" PRIu64 " used_bytes=%" PRIu32 " heap_bytes=%" PRIu32 "\r\n",
        run,
        ToNs(end - start),
        usedBytes,
        heapBytes);

    Emit(line, n, sizeof(line));
}
