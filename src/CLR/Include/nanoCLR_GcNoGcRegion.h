//
// Copyright (c) .NET Foundation and Contributors
// See LICENSE file in the project root for full license information.
//

// No-GC region: while active, no GC work of any kind runs (stop-the-world collection, incremental cycle work, heap
// compaction); allocation never collects, and a full heap throws OutOfMemoryException. Compiled in only when
// NANOCLR_GC_NOGC_REGION is defined (targets/posix: NANO_POSIX_GC_NOGC_REGION); otherwise the macros below expand to
// nothing or to false. Design: qflat docs/internal/agent/tasks/0003-no-gc-region.md.

#ifndef NANOCLR_GCNOGCREGION_H
#define NANOCLR_GCNOGCREGION_H

#if defined(NANOCLR_GC_NOGC_REGION)

#include <stdint.h>

#if defined(__GNUC__)
extern __attribute__((visibility("hidden"))) bool g_GcNoGcRegion_Active;
#else
extern bool g_GcNoGcRegion_Active;
#endif

// Starts a region of budget bytes after a collection: stops incremental GC work and checks free memory (total, not
// contiguous, so an upper bound). started is false when free memory is below bytes; CLR_E_OUT_OF_RANGE for bytes <= 0,
// CLR_E_INVALID_OPERATION if one is active. Debug-only forced collections (NANOCLR_FORCE_GC_BEFORE_EVERY_ALLOCATION,
// --forcegc) still run and break the region.
HRESULT GcNoGcRegion_Start(CLR_INT64 bytes, bool &started);
// Ends the region: true if it was active, within budget and not broken by a collection. False if none was active.
bool GcNoGcRegion_End();
// The region is active, or was broken by a collection and not yet ended.
bool GcNoGcRegion_InRegion();
// GC work units run so far: full collections, compactions and incremental cycle steps (diagnostics).
uint64_t GcNoGcRegion_WorkCount();
void GcNoGcRegion_CountWork();
// An allocation of length blocks inside the region: counted against the budget.
void GcNoGcRegion_OnAllocate(CLR_UINT32 length);
// A full collection was requested inside the region: it ends the region, which is reported as broken.
void GcNoGcRegion_OnCollect();

#define NANOCLR_GC_NOGC_ACTIVE() (g_GcNoGcRegion_Active)
// In ExtractHeapBlocks: a failed allocation returns null instead of collecting.
#define NANOCLR_GC_NOGC_NO_COLLECT(flags)                                                                              \
    {                                                                                                                  \
        if (g_GcNoGcRegion_Active)                                                                                     \
        {                                                                                                              \
            (flags) |= CLR_RT_HeapBlock::HB_NoGcOnFailure;                                                             \
        }                                                                                                              \
    }
// In CLR_RT_HeapCluster::ExtractBlocks, once blocks are taken: count them against the budget (event-cache hits and
// failed attempts are not counted).
#define NANOCLR_GC_NOGC_ON_EXTRACTED(length)                                                                           \
    {                                                                                                                  \
        if (g_GcNoGcRegion_Active)                                                                                     \
        {                                                                                                              \
            GcNoGcRegion_OnAllocate(length);                                                                           \
        }                                                                                                              \
    }
#define NANOCLR_GC_NOGC_ON_COLLECT() GcNoGcRegion_OnCollect();
#define NANOCLR_GC_COUNT_WORK()      GcNoGcRegion_CountWork();

#else

#define NANOCLR_GC_NOGC_ACTIVE() (false)
#define NANOCLR_GC_NOGC_NO_COLLECT(flags)
#define NANOCLR_GC_NOGC_ON_EXTRACTED(length)
#define NANOCLR_GC_NOGC_ON_COLLECT()
#define NANOCLR_GC_COUNT_WORK()

#endif // NANOCLR_GC_NOGC_REGION

#endif // NANOCLR_GCNOGCREGION_H
