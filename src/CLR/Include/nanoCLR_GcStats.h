//
// Copyright (c) .NET Foundation and Contributors
// See LICENSE file in the project root for full license information.
//

// GC statistics. Every outermost GC work unit (collection, compaction, incremental step) is counted by kind. A pause is
// the time a thread waits for GC work at one point: the span of the units run at one quantum boundary, idle share,
// allocation hook or profile change (NANOCLR_GC_STATS_PAUSE), or a unit run anywhere else. Blocks handed out by the
// allocator are counted. Compiled in only when NANOCLR_GC_NOGC_REGION is defined (targets/posix:
// NANO_POSIX_GC_NOGC_REGION), for QFlat.Device's GcPolicy; otherwise the macros expand to nothing. Design: qflat
// docs/internal/agent/tasks/0004-gc-stats-and-policy-api.md.

#ifndef NANOCLR_GCSTATS_H
#define NANOCLR_GCSTATS_H

#if defined(NANOCLR_GC_NOGC_REGION)

#include <stdint.h>

enum class GcStatsKind
{
    Collection,      // PerformGarbageCollection, including an incremental cycle's final pause
    Compaction,      // PerformHeapCompaction
    IncrementalStep, // initial pause, mark slice, pace step that scanned, sweep step, finished or on-demand sweep
};

// Indexes into GcStats_Get's array; QFlat.Device's GcPolicy reads them in this order.
enum GcStatsIndex
{
    GcStatsIndex_Collections,
    GcStatsIndex_Compactions,
    GcStatsIndex_IncrementalSteps,
    GcStatsIndex_CyclesCompleted,
    GcStatsIndex_CyclesAborted,
    GcStatsIndex_WorkUnits,
    GcStatsIndex_LastPauseMicroseconds,
    GcStatsIndex_MaxPauseMicroseconds,
    GcStatsIndex_TotalPauseMicroseconds,
    GcStatsIndex_HeapBytes,
    GcStatsIndex_FreeBytes,
    GcStatsIndex_AllocatedBytes,
    GcStatsIndex_Count
};

// One GC work unit: counted if outermost; timed as a pause too unless a GcStatsPauseScope encloses it.
class GcStatsScope
{
  public:
    explicit GcStatsScope(GcStatsKind kind);
    ~GcStatsScope();

  private:
    uint64_t m_startNs;
    GcStatsKind m_kind;
    bool m_outer;
};

// A point where a thread waits for whatever GC work runs there: one pause, from the first unit's start to the scope's
// end, recorded only if work ran. Cheap when none does (no clock read).
class GcStatsPauseScope
{
  public:
    GcStatsPauseScope();
    ~GcStatsPauseScope();

  private:
    bool m_outer;
};

// Fills values[0..count) in GcStatsIndex order (count is clamped to GcStatsIndex_Count).
void GcStats_Get(CLR_INT64 *values, CLR_UINT32 count);
// Last, max and total pause back to 0.
void GcStats_ResetPauses();
uint64_t GcStats_WorkUnits();
// Blocks handed out by the allocator since startup.
uint64_t GcStats_AllocatedBlocks();
void GcStats_OnExtracted(CLR_UINT32 length);
void GcStats_OnCycleCompleted();
// An incremental cycle is being discarded: counted, and its bitmap clearing is part of an enclosing pause.
void GcStats_OnCycleAborted();

#define NANOCLR_GC_STATS_SCOPE(kind)          GcStatsScope gcStatsScope_(GcStatsKind::kind);
#define NANOCLR_GC_STATS_PAUSE()              GcStatsPauseScope gcStatsPause_;
#define NANOCLR_GC_STATS_ON_EXTRACTED(length) GcStats_OnExtracted(length);
#define NANOCLR_GC_STATS_ON_CYCLE_COMPLETED() GcStats_OnCycleCompleted();
#define NANOCLR_GC_STATS_ON_CYCLE_ABORTED()   GcStats_OnCycleAborted();

#else

#define NANOCLR_GC_STATS_SCOPE(kind)
#define NANOCLR_GC_STATS_PAUSE()
#define NANOCLR_GC_STATS_ON_EXTRACTED(length)
#define NANOCLR_GC_STATS_ON_CYCLE_COMPLETED()
#define NANOCLR_GC_STATS_ON_CYCLE_ABORTED()

#endif // NANOCLR_GC_NOGC_REGION

#endif // NANOCLR_GCSTATS_H
