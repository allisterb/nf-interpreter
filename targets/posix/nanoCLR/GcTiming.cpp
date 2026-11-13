//
// Copyright (c) .NET Foundation and Contributors
// See LICENSE file in the project root for full license information.
//

// GC timing hooks (NANO_POSIX_GC_TIMING). Each record is one "[GCT]" line on the debug output, so it
// is ordered with Debug.WriteLine output. Durations are in nanoseconds. On QNX x86_64 ClockCycles()
// reads the TSC, so a thread moving between CPUs with unsynchronised TSCs can produce a wrapped
// (huge) duration; pin the process to one CPU (runmask) if that shows up.

#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
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

// Quantum samples are kept in ticks and summarised every c_QuantumBatch expired quanta.
const uint32_t c_QuantumBatch = 512;

struct QuantumSamples
{
    uint64_t run[c_QuantumBatch];           // start to end, expired quanta
    uint64_t timer[c_QuantumBatch];         // start to the timer firing
    uint64_t over[c_QuantumBatch];          // the timer firing past its armed length
    uint64_t notice[c_QuantumBatch];        // the timer firing to Execute returning
    uint64_t boundary[c_QuantumBatch];      // Execute returning to end, expired quanta
    uint64_t gap[c_QuantumBatch];           // the previous run's end to this start, expired quanta
    uint64_t shortBoundary[c_QuantumBatch]; // the boundary of runs that ended before their quantum
    uint64_t shortGap[c_QuantumBatch];      // the gap before those runs
    uint32_t count;
    uint32_t matched;  // expired quanta whose firing stamp fell inside the run
    uint32_t deadline; // expired quanta armed for less than the full quantum
    uint32_t gaps;
    uint32_t shortCount; // all short runs; only the first c_QuantumBatch are kept
    uint32_t shortGaps;
    uint32_t batch;
    uint64_t lastEmit; // the previous batch's emit, in ticks
};

std::atomic<uint64_t> s_quantumFired{0};
QuantumSamples s_quantum;
uint64_t s_lastQuantumEnd = 0;
uint32_t s_quantumArmedMs = CLR_RT_Thread::c_TimeQuantum_Milliseconds;

// Appends " name_us=median/p99/max" for the first n values, sorting them in place.
int AppendDistribution(char *out, size_t capacity, const char *name, uint64_t *values, uint32_t n)
{
    if (n == 0)
    {
        return std::snprintf(out, capacity, " %s_us=-", name);
    }

    std::qsort(values, n, sizeof(uint64_t), [](const void *a, const void *b) {
        uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
        return (x > y) - (x < y);
    });
    return std::snprintf(
        out,
        capacity,
        " %s_us=%" PRIu64 "/%" PRIu64 "/%" PRIu64,
        name,
        ToNs(values[n / 2]) / 1000,
        ToNs(values[(uint64_t)n * 99 / 100]) / 1000,
        ToNs(values[n - 1]) / 1000);
}

void EmitQuantumBatch()
{
    QuantumSamples &q = s_quantum;
    uint64_t emitStart = GcTiming_Now();
    char line[896];
    size_t used = 0;
    auto append = [&](int n) {
        if (n > 0)
        {
            size_t room = sizeof(line) - 1 - used;
            used += (size_t)n < room ? (size_t)n : room;
        }
    };

    append(std::snprintf(
        line,
        sizeof(line),
        "[QNT] batch=%" PRIu32 " quanta=%" PRIu32 " matched=%" PRIu32 " deadline=%" PRIu32 " short=%" PRIu32
        " last_emit_us=%" PRIu64,
        q.batch,
        q.count,
        q.matched,
        q.deadline,
        q.shortCount,
        ToNs(q.lastEmit) / 1000));
    append(AppendDistribution(line + used, sizeof(line) - used, "run", q.run, q.count));
    append(AppendDistribution(line + used, sizeof(line) - used, "timer", q.timer, q.matched));
    append(AppendDistribution(line + used, sizeof(line) - used, "timer_over", q.over, q.matched));
    append(AppendDistribution(line + used, sizeof(line) - used, "notice", q.notice, q.matched));
    append(AppendDistribution(line + used, sizeof(line) - used, "boundary", q.boundary, q.count));
    append(AppendDistribution(line + used, sizeof(line) - used, "gap", q.gap, q.gaps));
    append(AppendDistribution(
        line + used,
        sizeof(line) - used,
        "short_boundary",
        q.shortBoundary,
        q.shortCount < c_QuantumBatch ? q.shortCount : c_QuantumBatch));
    append(AppendDistribution(line + used, sizeof(line) - used, "short_gap", q.shortGap, q.shortGaps));
    append(std::snprintf(line + used, sizeof(line) - used, "\r\n"));

    Emit(line, (int)used, sizeof(line));

    q.count = q.matched = q.deadline = q.gaps = q.shortCount = q.shortGaps = 0;
    q.batch++;
    q.lastEmit = GcTiming_Now() - emitStart;
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

void GcTiming_QuantumArmed(uint32_t milliseconds)
{
    s_quantumArmedMs = milliseconds;
}

void GcTiming_QuantumFired()
{
    s_quantumFired.store(GcTiming_Now(), std::memory_order_relaxed);
    // Keeps the stamp ahead of the caller's plain flag store in hardware; the reader filters by window anyway.
    std::atomic_thread_fence(std::memory_order_release);
}

void GcTiming_ReportQuantum(uint64_t start, uint64_t returned, uint64_t end, bool expired)
{
    QuantumSamples &q = s_quantum;
    uint64_t lastEnd = s_lastQuantumEnd;
    bool hasGap = lastEnd != 0 && lastEnd <= start;
    s_lastQuantumEnd = end;

    if (!expired)
    {
        if (q.shortCount < c_QuantumBatch)
        {
            q.shortBoundary[q.shortCount] = end - returned;
            if (hasGap)
            {
                q.shortGap[q.shortGaps++] = start - lastEnd;
            }
        }
        q.shortCount++;
        return;
    }

    q.run[q.count] = end - start;
    q.boundary[q.count] = end - returned;
    q.count++;
    if (s_quantumArmedMs < CLR_RT_Thread::c_TimeQuantum_Milliseconds)
    {
        q.deadline++;
    }

    if (hasGap)
    {
        q.gap[q.gaps++] = start - lastEnd;
    }

    // A stamp outside the run belongs to another quantum; leave it out of the timer and notice columns.
    uint64_t fired = s_quantumFired.load(std::memory_order_relaxed);
    if (fired >= start && fired <= returned)
    {
        static const uint64_t ticksPerMs = TicksPerSecond() / 1000;
        uint64_t armed = s_quantumArmedMs * ticksPerMs;
        q.timer[q.matched] = fired - start;
        q.over[q.matched] = fired - start > armed ? fired - start - armed : 0;
        q.notice[q.matched] = returned - fired;
        q.matched++;
    }

    if (q.count == c_QuantumBatch)
    {
        EmitQuantumBatch();
    }
}
