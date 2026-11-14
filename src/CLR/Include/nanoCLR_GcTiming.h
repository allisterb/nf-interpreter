//
// Copyright (c) .NET Foundation and Contributors
// See LICENSE file in the project root for full license information.
//

// Optional per-phase timing of garbage collection and heap compaction, for measuring pauses on host
// builds. Compiled in only when NANOCLR_GC_TIMING is defined (targets/posix: NANO_POSIX_GC_TIMING);
// the target supplies the functions below.

#ifndef NANOCLR_GCTIMING_H
#define NANOCLR_GCTIMING_H

#if defined(NANOCLR_GC_TIMING)

#include <stdint.h>

// Indexes into the timestamp array passed to GcTiming_ReportCollection. Each stamp is taken after the
// named phase; c_GcTiming_Start is taken before the first.
enum GcTimingStamp
{
    c_GcTiming_Start,
    c_GcTiming_Prepare, // ExecutionConstraint_Suspend + EventCache_Cleanup
    c_GcTiming_Mark,    // Mark, including any MarkSlow passes
    c_GcTiming_MarkWeak,
    c_GcTiming_Sweep,
    c_GcTiming_Ratio, // Heap_ComputeAliveVsDeadRatio
    c_GcTiming_Pressure,
    c_GcTiming_Count
};

// Mark-stack overflow chunks allocated during one collection (the first 128 entries are on the C stack).
struct GcTimingMarkStack
{
    uint32_t chunks;         // chunks allocated
    uint32_t entries;        // entries in those chunks, sentinel included
    uint32_t failedAttempts; // allocation attempts that failed or were refused by the host cap
    uint64_t allocTicks;     // time spent allocating chunks, in target ticks
};

extern GcTimingMarkStack g_GcTimingMarkStack;

// Monotonic timestamp in target ticks.
uint64_t GcTiming_Now();

// Reports one collection. usedBytes and heapBytes are the collector's counters after the sweep.
void GcTiming_ReportCollection(
    uint32_t run,
    const uint64_t (&stamps)[c_GcTiming_Count],
    uint32_t markSlowPasses,
    const GcTimingMarkStack &markStack,
    uint32_t usedBytes,
    uint32_t heapBytes);

// Reports one compaction, timed from before ExecutionConstraint_Suspend to after ExecutionConstraint_Resume.
// usedBytes and heapBytes are as of the last collection: compaction does not update them.
void GcTiming_ReportCompaction(uint32_t run, uint64_t start, uint64_t end, uint32_t usedBytes, uint32_t heapBytes);

// Quantum timing. The target's quantum timer calls GcTiming_QuantumFired just before it sets the expired flag;
// ScheduleThreads reports each thread run: start before Execute, returned after it, end after the quantum-boundary
// work (GC quantum hook and ProcessTimer). expired is the thread's quantum flag when Execute returned.
void GcTiming_QuantumArmed(uint32_t milliseconds); // on the CLR thread, when Execute arms the timer
void GcTiming_QuantumFired();
void GcTiming_ReportQuantum(uint64_t start, uint64_t returned, uint64_t end, bool expired);

// Allocation timing. ExtractHeapBlocks brackets each allocation (length in heap blocks) with a GcTimingAllocScope;
// nested allocations fold into the outer one. CLR_RT_HeapCluster::ExtractBlocks reports each cluster search: start,
// after the free-list search, at the end, and the free-list nodes visited. Its corrupt-list early returns are not
// reported.
void GcTiming_AllocBegin(uint32_t length);
void GcTiming_AllocEnd();
void GcTiming_ReportExtract(uint64_t start, uint64_t searched, uint64_t end, uint32_t nodes);

struct GcTimingAllocScope
{
    explicit GcTimingAllocScope(uint32_t length)
    {
        GcTiming_AllocBegin(length);
    }
    ~GcTimingAllocScope()
    {
        GcTiming_AllocEnd();
    }
    GcTimingAllocScope(const GcTimingAllocScope &) = delete;
    GcTimingAllocScope &operator=(const GcTimingAllocScope &) = delete;
};

#endif // NANOCLR_GC_TIMING

#endif // NANOCLR_GCTIMING_H
