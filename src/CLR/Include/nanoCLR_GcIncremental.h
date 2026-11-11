//
// Copyright (c) .NET Foundation and Contributors
// See LICENSE file in the project root for full license information.
//

// Hooks for an incremental, snapshot-at-the-beginning (Yuasa) collector. Compiled in only when
// NANOCLR_GC_INCREMENTAL is defined (targets/posix: NANO_POSIX_GC_INCREMENTAL); otherwise every macro below
// expands to nothing, so call sites take no semicolon. Rationale and the store-site audit behind each hook:
// qflat docs/internal/agent/tasks/0002-incremental-gc-spike.md.

#ifndef NANOCLR_GCINCREMENTAL_H
#define NANOCLR_GCINCREMENTAL_H

#if defined(NANOCLR_GC_INCREMENTAL)

#include <stddef.h>

struct CLR_RT_HeapBlock;

// True while the deletion barrier must run (a mark cycle is active, or barrier stress mode). Hidden so the inline
// check in every setter is a direct load rather than a GOT indirection in the shared library.
#if defined(__GNUC__)
extern __attribute__((visibility("hidden"))) bool g_GcIncremental_BarrierActive;
#else
extern bool g_GcIncremental_BarrierActive;
#endif

// Deletion barrier: before *slot is overwritten, shade the object it references.
void GcIncremental_PreWrite(const CLR_RT_HeapBlock *slot);
// Deletion barrier over raw bytes about to be overwritten (memcpy/memset destinations).
void GcIncremental_ShadeBytes(const void *dst, size_t bytes);
// Keep-alive barrier: obj was just read through a weak or untraced pointer.
void GcIncremental_ShadeObject(const CLR_RT_HeapBlock *obj);
// True if block was allocated during the current cycle (allocated black; its old contents are never shaded).
bool GcIncremental_IsAllocatedThisCycle(const CLR_RT_HeapBlock *block);

// Heap bookkeeping: object-start and allocated-this-cycle bitmaps.
void GcIncremental_OnAllocate(const CLR_RT_HeapBlock *block, CLR_UINT32 flags);
void GcIncremental_OnFree(const CLR_RT_HeapBlock *block, CLR_UINT32 length);
void GcIncremental_OnHeapInitialized();
void GcIncremental_OnHeapCompacted();
// Start of a stop-the-world mark.
void GcIncremental_OnFullMark();
// Mark() has finished: verify mode checks its result here.
void GcIncremental_OnMarked();

// Incremental cycle. FinishCycle runs in place of Mark() in a full collection: with a cycle active it completes the
// cycle's mark (drain, finalizer pass, transfer to HB_Alive) and returns true. It returns false, and Mark() runs, when
// no cycle is active or the cycle lost a grey push.
bool GcIncremental_FinishCycle();
// A thread quantum has ended: start a cycle, run a mark slice, or request the final pause.
void GcIncremental_OnQuantum();
// Heap compaction is about to move objects: abort any active cycle.
void GcIncremental_OnCompaction();

#define NANOCLR_GC_PRE_WRITE(slot)                                                                                     \
    {                                                                                                                  \
        if (g_GcIncremental_BarrierActive)                                                                             \
        {                                                                                                              \
            GcIncremental_PreWrite(slot);                                                                              \
        }                                                                                                              \
    }
#define NANOCLR_GC_SHADE_BYTES(dst, bytes)                                                                             \
    {                                                                                                                  \
        if (g_GcIncremental_BarrierActive)                                                                             \
        {                                                                                                              \
            GcIncremental_ShadeBytes(dst, bytes);                                                                      \
        }                                                                                                              \
    }
#define NANOCLR_GC_SHADE_BYTES_IF(cond, dst, bytes)                                                                    \
    {                                                                                                                  \
        if (g_GcIncremental_BarrierActive && (cond))                                                                   \
        {                                                                                                              \
            GcIncremental_ShadeBytes(dst, bytes);                                                                      \
        }                                                                                                              \
    }
#define NANOCLR_GC_SHADE_OBJECT(obj)                                                                                   \
    {                                                                                                                  \
        if (g_GcIncremental_BarrierActive)                                                                             \
        {                                                                                                              \
            GcIncremental_ShadeObject(obj);                                                                            \
        }                                                                                                              \
    }
#define NANOCLR_GC_ON_ALLOCATE(block, flags) GcIncremental_OnAllocate(block, flags);
#define NANOCLR_GC_ON_FREE(block, length)    GcIncremental_OnFree(block, length);
#define NANOCLR_GC_ON_HEAP_INITIALIZED()     GcIncremental_OnHeapInitialized();
#define NANOCLR_GC_ON_HEAP_COMPACTED()       GcIncremental_OnHeapCompacted();
#define NANOCLR_GC_ON_FULL_MARK()            GcIncremental_OnFullMark();
#define NANOCLR_GC_ON_MARKED()               GcIncremental_OnMarked();
#define NANOCLR_GC_ON_QUANTUM()              GcIncremental_OnQuantum();
#define NANOCLR_GC_ON_COMPACTION()           GcIncremental_OnCompaction();
#define NANOCLR_GC_MARK()                                                                                              \
    {                                                                                                                  \
        if (!GcIncremental_FinishCycle())                                                                              \
        {                                                                                                              \
            Mark();                                                                                                    \
        }                                                                                                              \
    }

#else

#define NANOCLR_GC_PRE_WRITE(slot)
#define NANOCLR_GC_SHADE_BYTES(dst, bytes)
#define NANOCLR_GC_SHADE_BYTES_IF(cond, dst, bytes)
#define NANOCLR_GC_SHADE_OBJECT(obj)
#define NANOCLR_GC_ON_ALLOCATE(block, flags)
#define NANOCLR_GC_ON_FREE(block, length)
#define NANOCLR_GC_ON_HEAP_INITIALIZED()
#define NANOCLR_GC_ON_HEAP_COMPACTED()
#define NANOCLR_GC_ON_FULL_MARK()
#define NANOCLR_GC_ON_MARKED()
#define NANOCLR_GC_ON_QUANTUM()
#define NANOCLR_GC_ON_COMPACTION()
#define NANOCLR_GC_MARK() Mark();

#endif // NANOCLR_GC_INCREMENTAL

#endif // NANOCLR_GCINCREMENTAL_H
