//
// Copyright (c) .NET Foundation and Contributors
// See LICENSE file in the project root for full license information.
//

// GC statistics: see nanoCLR_GcStats.h and qflat docs/internal/agent/tasks/0004-gc-stats-and-policy-api.md.

#include "Core.h"

#include <time.h>

namespace
{

CLR_UINT32 s_depth;      // GC work units in progress (nested ones are part of the outermost)
CLR_UINT32 s_pauseDepth; // GcStatsPauseScopes in progress
uint64_t s_pauseStartNs; // the enclosing pause's first work, or 0 if none has run yet
uint64_t s_counts[3];    // per GcStatsKind
uint64_t s_cyclesCompleted;
uint64_t s_cyclesAborted;
uint64_t s_lastPauseNs;
uint64_t s_maxPauseNs;
uint64_t s_totalPauseNs;
uint64_t s_allocatedBlocks;

uint64_t NowNs()
{
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);

    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

void RecordPause(uint64_t elapsed)
{
    s_lastPauseNs = elapsed;
    s_totalPauseNs += elapsed;

    if (elapsed > s_maxPauseNs)
    {
        s_maxPauseNs = elapsed;
    }
}

// Work is starting: an enclosing pause scope's span starts here unless earlier work started it.
void NoteWorkStart(uint64_t now)
{
    if (s_pauseDepth != 0 && s_pauseStartNs == 0)
    {
        s_pauseStartNs = now;
    }
}

} // namespace

GcStatsScope::GcStatsScope(GcStatsKind kind) : m_startNs(0), m_kind(kind), m_outer(s_depth++ == 0)
{
    if (m_outer)
    {
        m_startNs = NowNs();
        NoteWorkStart(m_startNs);
    }
}

GcStatsScope::~GcStatsScope()
{
    s_depth--;

    if (!m_outer)
    {
        return;
    }

    s_counts[(int)m_kind]++;

    // Inside a pause scope, the scope records the whole span instead.
    if (s_pauseDepth == 0)
    {
        RecordPause(NowNs() - m_startNs);
    }
}

GcStatsPauseScope::GcStatsPauseScope() : m_outer(s_pauseDepth++ == 0)
{
    if (m_outer)
    {
        s_pauseStartNs = 0;
    }
}

GcStatsPauseScope::~GcStatsPauseScope()
{
    s_pauseDepth--;

    if (m_outer && s_pauseStartNs != 0)
    {
        RecordPause(NowNs() - s_pauseStartNs);
        s_pauseStartNs = 0;
    }
}

void GcStats_Get(CLR_INT64 *values, CLR_UINT32 count)
{
    const CLR_RT_GarbageCollector &gc = g_CLR_RT_GarbageCollector;
    const CLR_INT64 all[GcStatsIndex_Count] = {
        (CLR_INT64)s_counts[(int)GcStatsKind::Collection],
        (CLR_INT64)s_counts[(int)GcStatsKind::Compaction],
        (CLR_INT64)s_counts[(int)GcStatsKind::IncrementalStep],
        (CLR_INT64)s_cyclesCompleted,
        (CLR_INT64)s_cyclesAborted,
        (CLR_INT64)GcStats_WorkUnits(),
        (CLR_INT64)(s_lastPauseNs / 1000),
        (CLR_INT64)(s_maxPauseNs / 1000),
        (CLR_INT64)(s_totalPauseNs / 1000),
        (CLR_INT64)gc.m_totalBytes,
        (CLR_INT64)gc.m_freeBytes,
        (CLR_INT64)(s_allocatedBlocks * sizeof(CLR_RT_HeapBlock)),
    };

    for (CLR_UINT32 i = 0; i < count && i < GcStatsIndex_Count; i++)
    {
        values[i] = all[i];
    }
}

void GcStats_ResetPauses()
{
    s_lastPauseNs = 0;
    s_maxPauseNs = 0;
    s_totalPauseNs = 0;
}

uint64_t GcStats_WorkUnits()
{
    return s_counts[0] + s_counts[1] + s_counts[2];
}

uint64_t GcStats_AllocatedBlocks()
{
    return s_allocatedBlocks;
}

void GcStats_OnExtracted(CLR_UINT32 length)
{
    s_allocatedBlocks += length;
}

void GcStats_OnCycleCompleted()
{
    s_cyclesCompleted++;
}

void GcStats_OnCycleAborted()
{
    s_cyclesAborted++;

    // Inside a unit, the abort is part of that unit's time.
    if (s_depth == 0)
    {
        NoteWorkStart(NowNs());
    }
}
