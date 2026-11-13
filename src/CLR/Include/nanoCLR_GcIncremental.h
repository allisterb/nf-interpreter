//
// Copyright (c) .NET Foundation and Contributors
// See LICENSE file in the project root for full license information.
//

// Hooks for an incremental, snapshot-at-the-beginning (Yuasa) collector. Compiled in only when
// NANOCLR_GC_INCREMENTAL is defined (targets/posix: NANO_POSIX_GC_INCREMENTAL); otherwise every macro below
// expands to nothing, so call sites take no semicolon. NANOCLR_GC_ON_SWEEP_HEAP() expands to an `if` guarding the
// block that follows it; NANOCLR_GC_ON_ALLOCATION_FAILED() returns from the caller with the block it found. Rationale
// and the store-site audit behind each hook:
// qflat docs/internal/agent/tasks/0002-incremental-gc-spike.md.

#ifndef NANOCLR_GCINCREMENTAL_H
#define NANOCLR_GCINCREMENTAL_H

#if defined(NANOCLR_GC_INCREMENTAL)

#include <stddef.h>
#include <stdint.h>

struct CLR_RT_HeapBlock;

// True while the deletion barrier must run (a mark cycle is active, or barrier stress mode). Hidden so the inline
// check in every setter is a direct load rather than a GOT indirection in the shared library.
#if defined(__GNUC__)
extern __attribute__((visibility("hidden"))) bool g_GcIncremental_BarrierActive;
#else
extern bool g_GcIncremental_BarrierActive;
#endif

// Live bitmap of the incremental cycle being swept, or null. Published from the end of the final pause's mark until
// the last heap cluster is swept, which can be after the pause (verify mode unpublishes it earlier, after copying it
// into HB_Alive), so MarkWeak() and the sweep read the cycle's mark through IsAlive() instead of a heap walk copying it
// into HB_Alive. Bit i covers the block at g_GcIncremental_LiveBase + i blocks.
#if defined(__GNUC__)
extern __attribute__((visibility("hidden"))) const CLR_UINT32 *g_GcIncremental_LiveBits;
extern __attribute__((visibility("hidden"))) uintptr_t g_GcIncremental_LiveBase;
extern __attribute__((visibility("hidden"))) size_t g_GcIncremental_LiveSlots;
#else
extern const CLR_UINT32 *g_GcIncremental_LiveBits;
extern uintptr_t g_GcIncremental_LiveBase;
extern size_t g_GcIncremental_LiveSlots;
#endif

// True if the block at p, of blockSize bytes, is set in g_GcIncremental_LiveBits (which must be non-null).
inline bool GcIncremental_IsLive(const void *p, size_t blockSize)
{
    const uintptr_t offset = (uintptr_t)p - g_GcIncremental_LiveBase;
    const size_t i = offset / blockSize;

    return offset % blockSize == 0 && i < g_GcIncremental_LiveSlots &&
           ((g_GcIncremental_LiveBits[i / 32] >> (i % 32)) & 1);
}

// While g_GcIncremental_LiveBits is published: marks block alive in it and returns true if block is an object start;
// otherwise returns false (the caller sets HB_Alive) and the block's heap cluster takes the header sweep (the whole
// heap does if there are too many such blocks, or one is off the block grid).
bool GcIncremental_SetLive(const CLR_RT_HeapBlock *block);

// Deletion barrier: before *slot is overwritten, shade the object it references.
void GcIncremental_PreWrite(const CLR_RT_HeapBlock *slot);
// Deletion barrier over raw bytes about to be overwritten (memcpy/memset destinations).
void GcIncremental_ShadeBytes(const void *dst, size_t bytes);
// Keep-alive barrier: obj was just read through a weak or untraced pointer.
void GcIncremental_ShadeObject(const CLR_RT_HeapBlock *obj);
// True if block was allocated during the current cycle (allocated black; its old contents are never shaded).
bool GcIncremental_IsAllocatedThisCycle(const CLR_RT_HeapBlock *block);

// Heap bookkeeping: object-start, event-start, block-end and allocated-this-cycle bitmaps.
void GcIncremental_OnAllocate(const CLR_RT_HeapBlock *block, CLR_UINT32 flags);
void GcIncremental_OnFree(const CLR_RT_HeapBlock *block, CLR_UINT32 length);
// The event cache split a cached event block after its first blocks blocks.
void GcIncremental_OnEventSplit(const CLR_RT_HeapBlock *block, CLR_UINT32 blocks);
// block's header is about to be restamped with size blocks (SetReflection, Type.GetTypeFromHandle). If block is an
// object start and that truncates it, its end bit moves; the cut-off slots (orphans) are left outside every block.
void GcIncremental_OnResize(const CLR_RT_HeapBlock *block, CLR_UINT32 blocks);
// block is joining the event cache (CLR_RT_EventCache::Append_Node): if it was allocated as an object, it becomes an
// event block in the bitmaps, so the bitmap sweep keeps it.
void GcIncremental_OnEventCached(const CLR_RT_HeapBlock *block);
void GcIncremental_OnHeapInitialized();
void GcIncremental_OnHeapCompacted();
// Start of a stop-the-world mark.
void GcIncremental_OnFullMark();
// Mark() has finished: verify mode checks its result here.
void GcIncremental_OnMarked();
// Sweep() is about to rebuild the free lists: from here to OnSwept, every free run is counted. After an incremental
// cycle it returns true and rebuilds them from the bitmaps one heap cluster at a time, walking the headers of any
// cluster whose bitmaps fail a check. The clusters are swept after the pause (OnQuantum, OnAllocationFailed, or at once
// before a collection, compaction or failed event allocation), or here with NANOCLR_GC_INCREMENTAL_SWEEP_US=0. It
// returns false when the cluster walks must run for the whole heap.
bool GcIncremental_OnSweepHeap();
// Sweep() has finished: an incremental cycle's live bitmap is retired and its "[GCC]" line kept for OnQuantum, here or
// when the sweep ends after the pause.
void GcIncremental_OnSwept();
// In place of Heap_ComputeAliveVsDeadRatio() after a sweep: sets m_totalBytes and m_freeBytes from the free runs the
// sweep counted, instead of walking every free-list node; a sweep continuing after the pause sets them when it ends.
// Returns false (the caller walks) if no count is available.
bool GcIncremental_ApplySweptFree();
// An allocation found no free block (outside a collection): sweeps the clusters an incremental sweep has not reached
// yet, trying an object allocation in each as it is swept (an event allocation, once all are swept, back to front as
// the allocator does). Returns the block, or null.
CLR_RT_HeapBlock *GcIncremental_OnAllocationFailed(CLR_UINT32 dataType, CLR_UINT32 flags, CLR_UINT32 length);

// Incremental cycle. FinishCycle runs in place of Mark() in a full collection: with a cycle active it completes the
// cycle's mark (drain, finalizer pass, publishing g_GcIncremental_LiveBits) and returns true. It returns false, and
// Mark() runs, when no cycle is active, the cycle lost a grey push, or the collection is not the cycle's own final
// pause (GC.Collect or an allocation failure mid-cycle discards the cycle for the exact stop-the-world result).
bool GcIncremental_FinishCycle();
// A thread quantum has ended: print the last cycle's pending "[GCC]" line, then run a sweep step, start a cycle, run a
// mark slice, or request the final pause.
void GcIncremental_OnQuantum();
// Heap compaction is about to move objects: abort any active cycle, and finish any sweep.
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
#define NANOCLR_GC_ON_EVENT_SPLIT(block, n)  GcIncremental_OnEventSplit(block, n);
#define NANOCLR_GC_ON_RESIZE(block, n)       GcIncremental_OnResize(block, n);
#define NANOCLR_GC_ON_EVENT_CACHED(block)    GcIncremental_OnEventCached(block);
// A whole slot id was copied onto a new object header: drop the HB_Alive it may carry, which no bitmap sweep clears.
#define NANOCLR_GC_ON_HEADER_COPIED(block) (block)->MarkDead();
#define NANOCLR_GC_ON_HEAP_INITIALIZED()   GcIncremental_OnHeapInitialized();
#define NANOCLR_GC_ON_HEAP_COMPACTED()     GcIncremental_OnHeapCompacted();
#define NANOCLR_GC_ON_FULL_MARK()          GcIncremental_OnFullMark();
#define NANOCLR_GC_ON_MARKED()             GcIncremental_OnMarked();
#define NANOCLR_GC_ON_SWEEP_HEAP()         if (!GcIncremental_OnSweepHeap())
#define NANOCLR_GC_ON_SWEPT()              GcIncremental_OnSwept();
#define NANOCLR_GC_ON_QUANTUM()            GcIncremental_OnQuantum();
#define NANOCLR_GC_ON_COMPACTION()         GcIncremental_OnCompaction();
#define NANOCLR_GC_MARK()                                                                                              \
    {                                                                                                                  \
        if (!GcIncremental_FinishCycle())                                                                              \
        {                                                                                                              \
            Mark();                                                                                                    \
        }                                                                                                              \
    }
#define NANOCLR_GC_COMPUTE_ALIVE_VS_DEAD_RATIO()                                                                       \
    {                                                                                                                  \
        if (!GcIncremental_ApplySweptFree())                                                                           \
        {                                                                                                              \
            Heap_ComputeAliveVsDeadRatio();                                                                            \
        }                                                                                                              \
    }
#define NANOCLR_GC_ON_ALLOCATION_FAILED(hb, dataType, flags, length)                                                   \
    {                                                                                                                  \
        if (((hb) = GcIncremental_OnAllocationFailed(dataType, flags, length)) != nullptr)                             \
        {                                                                                                              \
            return hb;                                                                                                 \
        }                                                                                                              \
    }

#else

#define NANOCLR_GC_PRE_WRITE(slot)
#define NANOCLR_GC_SHADE_BYTES(dst, bytes)
#define NANOCLR_GC_SHADE_BYTES_IF(cond, dst, bytes)
#define NANOCLR_GC_SHADE_OBJECT(obj)
#define NANOCLR_GC_ON_ALLOCATE(block, flags)
#define NANOCLR_GC_ON_FREE(block, length)
#define NANOCLR_GC_ON_EVENT_SPLIT(block, n)
#define NANOCLR_GC_ON_RESIZE(block, n)
#define NANOCLR_GC_ON_EVENT_CACHED(block)
#define NANOCLR_GC_ON_HEADER_COPIED(block)
#define NANOCLR_GC_ON_HEAP_INITIALIZED()
#define NANOCLR_GC_ON_HEAP_COMPACTED()
#define NANOCLR_GC_ON_FULL_MARK()
#define NANOCLR_GC_ON_MARKED()
#define NANOCLR_GC_ON_SWEEP_HEAP()
#define NANOCLR_GC_ON_SWEPT()
#define NANOCLR_GC_ON_QUANTUM()
#define NANOCLR_GC_ON_COMPACTION()
#define NANOCLR_GC_MARK()                        Mark();
#define NANOCLR_GC_COMPUTE_ALIVE_VS_DEAD_RATIO() Heap_ComputeAliveVsDeadRatio();
#define NANOCLR_GC_ON_ALLOCATION_FAILED(hb, dataType, flags, length)

#endif // NANOCLR_GC_INCREMENTAL

#endif // NANOCLR_GCINCREMENTAL_H
