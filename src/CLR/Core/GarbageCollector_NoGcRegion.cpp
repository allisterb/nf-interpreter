//
// Copyright (c) .NET Foundation and Contributors
// See LICENSE file in the project root for full license information.
//

// No-GC region: see nanoCLR_GcNoGcRegion.h and qflat docs/internal/agent/tasks/0003-no-gc-region.md.

#include "Core.h"

#include <stdlib.h>
#include <string.h>

#if defined(__GNUC__)
__attribute__((visibility("hidden"))) bool g_GcNoGcRegion_Active = false;
#else
bool g_GcNoGcRegion_Active = false;
#endif

namespace
{

bool s_broken;               // a collection ended the region before EndNoGcRegion
bool s_traced;               // NANOCLR_GC_NOGC_TRACE read
bool s_trace;                // print a [GCN] line at each start and end
uint64_t s_budgetBlocks;     // the region's declared budget
uint64_t s_allocatedAtStart; // GcStats_AllocatedBlocks() when the region started
uint64_t s_workAtStart;      // GcStats_WorkUnits() when the region started

// Blocks the allocator handed out since the region started (event-cache hits and failed attempts are not counted).
uint64_t UsedBlocks()
{
    return GcStats_AllocatedBlocks() - s_allocatedAtStart;
}

bool Trace()
{
    if (!s_traced)
    {
        const char *value = getenv("NANOCLR_GC_NOGC_TRACE");
        s_trace = value != nullptr && value[0] != '\0' && strcmp(value, "0") != 0;
        s_traced = true;
    }

    return s_trace;
}

} // namespace

HRESULT GcNoGcRegion_Start(CLR_INT64 bytes, bool &started)
{
    NATIVE_PROFILE_CLR_CORE();
    NANOCLR_HEADER();

    started = false;

    if (bytes <= 0)
    {
        NANOCLR_SET_AND_LEAVE(CLR_E_OUT_OF_RANGE);
    }

    if (g_GcNoGcRegion_Active || s_broken)
    {
        NANOCLR_SET_AND_LEAVE(CLR_E_INVALID_OPERATION);
    }

    // The caller has just collected (with compaction) and drained pending finalizers. A cycle started since (by the
    // finalizers' allocations) is aborted, and the sweep finished, so m_freeBytes is current and no GC work is left.
    NANOCLR_GC_ON_NOGC_START()

    if ((CLR_UINT64)bytes > g_CLR_RT_GarbageCollector.m_freeBytes)
    {
        if (Trace())
        {
            CLR_Debug::Printf(
                "[GCN] start refused: bytes=%llu free=%u\r\n",
                (unsigned long long)bytes,
                (unsigned)g_CLR_RT_GarbageCollector.m_freeBytes);
        }

        NANOCLR_SET_AND_LEAVE(S_OK);
    }

    s_budgetBlocks = ((CLR_UINT64)bytes + sizeof(CLR_RT_HeapBlock) - 1) / sizeof(CLR_RT_HeapBlock);
    s_allocatedAtStart = GcStats_AllocatedBlocks();
    s_workAtStart = GcStats_WorkUnits();
    g_GcNoGcRegion_Active = true;
    started = true;

    if (Trace())
    {
        CLR_Debug::Printf(
            "[GCN] start bytes=%llu free=%u\r\n",
            (unsigned long long)bytes,
            (unsigned)g_CLR_RT_GarbageCollector.m_freeBytes);
    }

    NANOCLR_NOCLEANUP();
}

bool GcNoGcRegion_End()
{
    NATIVE_PROFILE_CLR_CORE();

    if (!g_GcNoGcRegion_Active && !s_broken)
    {
        return false;
    }

    const bool held = g_GcNoGcRegion_Active && UsedBlocks() <= s_budgetBlocks;

    if (Trace())
    {
        CLR_Debug::Printf(
            "[GCN] end held=%u broken=%u used_bytes=%llu budget_bytes=%llu gc_work=%llu\r\n",
            (unsigned)held,
            (unsigned)s_broken,
            (unsigned long long)(UsedBlocks() * sizeof(CLR_RT_HeapBlock)),
            (unsigned long long)(s_budgetBlocks * sizeof(CLR_RT_HeapBlock)),
            (unsigned long long)(GcStats_WorkUnits() - s_workAtStart));
    }

    // Pending compaction runs at the next safe point, and the incremental collector's trigger, which kept counting,
    // starts a cycle at the next quantum boundary or allocation.
    g_GcNoGcRegion_Active = false;
    s_broken = false;

    return held;
}

bool GcNoGcRegion_InRegion()
{
    return g_GcNoGcRegion_Active || s_broken;
}

void GcNoGcRegion_OnCollect()
{
    if (!g_GcNoGcRegion_Active)
    {
        return;
    }

    if (Trace())
    {
        CLR_Debug::Printf(
            "[GCN] broken by a collection: used_bytes=%llu\r\n",
            (unsigned long long)(UsedBlocks() * sizeof(CLR_RT_HeapBlock)));
    }

    g_GcNoGcRegion_Active = false;
    s_broken = true;
}
