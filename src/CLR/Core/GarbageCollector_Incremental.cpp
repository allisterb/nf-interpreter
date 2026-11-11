//
// Copyright (c) .NET Foundation and Contributors
// See LICENSE file in the project root for full license information.
//

// Barrier and heap bookkeeping for an incremental snapshot-at-the-beginning collector (NANOCLR_GC_INCREMENTAL).
// See nanoCLR_GcIncremental.h.
//
// Shading never trusts the value it reads: a slot being overwritten may hold stale or uninitialised data (C-stack
// temporaries, eval-stack slots above the top, freshly extracted blocks). A target is shaded only if it is the
// start of a live object according to the object-start bitmap, which ExtractBlocks sets and every path that frees
// heap memory clears. A false positive can then only shade a real, allocated object, which survives one more cycle.
// Its fields are trusted by the marker like any other object's, so the incremental marker (task 0002 step 4) must
// not see objects whose fields are not yet initialised: allocation stamps fields before anything can yield.
//
// Barrier stress mode (environment variable NANOCLR_GC_BARRIER_STRESS=1) turns the barrier on for the whole run
// without changing collection: shades are validated and counted, never marked, and each full collection checks the
// object-start bitmap against a heap walk and prints one "[GCI]" line.
//
// Verify mode (NANOCLR_GC_VERIFY=1) checks a mark result against the stop-the-world mark after every Mark(): it
// snapshots the marked objects, unmarks them, marks again from the roots and counts every object the second mark
// finds live that the snapshot lacks (a miss), printing the first few, then one "[GCV]" line. The union of both marks
// is kept (relinking restored weak delegate lists), so a miss never frees a live object. Until the incremental marker
// exists (task 0002 step 4) the mark under test is the stop-the-world mark itself; NANOCLR_GC_VERIFY_INJECT=N drops
// every Nth marked object from the snapshot to test the detector. A dropped object that is still reachable comes back
// as a miss; one the second mark does not reach (an unreachable block that was already HB_Alive) is counted as
// dropped_dead, so misses + dropped_dead == injected on every collection.

#include "Core.h"

#if defined(NANOCLR_GC_INCREMENTAL)

#include <stdlib.h>
#include <string.h>

bool g_GcIncremental_BarrierActive;

namespace
{

struct GcIncrementalStats
{
    CLR_UINT32 preWrites;    // slow-path barrier calls on a single slot
    CLR_UINT32 shadeBytes;   // raw-copy range shades
    CLR_UINT32 weakShades;   // keep-alive shades on weak reads
    CLR_UINT32 pushed;       // targets pushed grey
    CLR_UINT32 alreadyBlack; // targets already marked or allocated this cycle
    CLR_UINT32 rejected;     // non-null targets that are not a live object start
    CLR_UINT32 greyDropped;  // pushes lost because the grey stack could not grow
    CLR_UINT32 dirtyAllocs;  // stress mode: allocations whose memory still carried bitmap bits
};

const size_t c_slotSize = sizeof(CLR_RT_HeapBlock);
const size_t c_bitsPerWord = 32;
const size_t c_stressGreyCap = 65536;
const CLR_UINT32 c_verifyReportedMisses = 5;   // misses printed in detail per collection
const CLR_UINT32 c_verifyReportedFloating = 3; // floating objects printed in detail per collection

bool s_valid;
bool s_stress;
bool s_verify;
CLR_UINT32 s_verifyInject; // drop every Nth marked object from the verify snapshot; 0 = never
CLR_UINT32 *s_snapshot;    // verify mode: objects the mark under test marked
CLR_UINT32 *s_dropped;     // verify mode with injection: marked objects dropped from the snapshot
uintptr_t s_base;          // address of slot 0: the first cluster's payload start
uintptr_t s_end;
size_t s_slots;
CLR_UINT32 *s_starts;    // object-start bitmap
CLR_UINT32 *s_allocated; // allocated-this-cycle bitmap
const CLR_RT_HeapBlock **s_grey;
size_t s_greyCount;
size_t s_greyCapacity;
GcIncrementalStats s_stats;

bool TestBit(const CLR_UINT32 *bits, size_t i)
{
    return (bits[i / c_bitsPerWord] >> (i % c_bitsPerWord)) & 1;
}

void SetBit(CLR_UINT32 *bits, size_t i)
{
    bits[i / c_bitsPerWord] |= 1u << (i % c_bitsPerWord);
}

// Bits [from, 32) of a word; from < 32.
CLR_UINT32 MaskFrom(size_t from)
{
    return ~0u << from;
}

void ClearBits(CLR_UINT32 *bits, size_t first, size_t count)
{
    if (count == 0)
    {
        return;
    }

    const size_t last = first + count - 1;
    size_t w = first / c_bitsPerWord;
    const size_t lastWord = last / c_bitsPerWord;
    // Bits up to and including last % 32 of the last word.
    const CLR_UINT32 tail = ~(MaskFrom(last % c_bitsPerWord) << 1);

    if (w == lastWord)
    {
        bits[w] &= ~(MaskFrom(first % c_bitsPerWord) & tail);
        return;
    }

    bits[w++] &= ~MaskFrom(first % c_bitsPerWord);

    for (; w < lastWord; w++)
    {
        bits[w] = 0;
    }

    bits[lastWord] &= ~tail;
}

size_t BitmapWords()
{
    return (s_slots + c_bitsPerWord - 1) / c_bitsPerWord;
}

bool InHeap(const void *p)
{
    return (uintptr_t)p >= (uintptr_t)s_CLR_RT_Heap.location &&
           (uintptr_t)p < (uintptr_t)(s_CLR_RT_Heap.location + s_CLR_RT_Heap.size);
}

// Slot index of p if p lies on the block grid inside the heap, else -1.
ptrdiff_t SlotIndex(const void *p)
{
    uintptr_t a = (uintptr_t)p;

    if (a < s_base || a >= s_end || (a - s_base) % c_slotSize != 0 || (a - s_base) / c_slotSize >= s_slots)
    {
        return -1;
    }

    return (ptrdiff_t)((a - s_base) / c_slotSize);
}

bool IsObjectBlock(const CLR_RT_HeapBlock *block)
{
    NanoCLRDataType dt = block->DataType();

    return dt != DATATYPE_FREEBLOCK && dt != DATATYPE_CACHEDBLOCK && dt < DATATYPE_FIRST_INVALID && !block->IsEvent();
}

void PushGrey(const CLR_RT_HeapBlock *obj)
{
    if (s_greyCount == s_greyCapacity)
    {
        size_t capacity = s_greyCapacity ? s_greyCapacity * 2 : 1024;
        auto *grey = (const CLR_RT_HeapBlock **)(s_stress && capacity > c_stressGreyCap
                                                     ? nullptr
                                                     : platform_malloc(capacity * sizeof(*s_grey)));

        if (grey == nullptr)
        {
            s_stats.greyDropped++;
            return;
        }

        if (s_grey)
        {
            memcpy(grey, s_grey, s_greyCount * sizeof(*grey));
            platform_free(s_grey);
        }

        s_grey = grey;
        s_greyCapacity = capacity;
    }

    s_grey[s_greyCount++] = obj;
    s_stats.pushed++;
}

void Shade(const CLR_RT_HeapBlock *obj)
{
    if (obj == nullptr)
    {
        return;
    }

    ptrdiff_t i = s_valid ? SlotIndex(obj) : -1;

    if (i < 0 || !TestBit(s_starts, (size_t)i) || !IsObjectBlock(obj))
    {
        s_stats.rejected++;
        return;
    }

    if (obj->IsAlive() || TestBit(s_allocated, (size_t)i))
    {
        s_stats.alreadyBlack++;
        return;
    }

    PushGrey(obj);
}

// Shades the reference held in slot, if any. Heap slots hold OBJECT references (fields, reference-array elements,
// delegate targets); BYREF and ARRAY_BYREF reach the heap only through ref fields of ref structs, and are shaded too
// when they point at an object start.
void ShadeSlot(const CLR_RT_HeapBlock *slot)
{
    switch (slot->DataType())
    {
        case DATATYPE_OBJECT:
        case DATATYPE_BYREF:
            Shade(slot->Dereference());
            break;

        case DATATYPE_ARRAY_BYREF:
            Shade((const CLR_RT_HeapBlock *)slot->Array());
            break;

        default:
            break;
    }
}

void RebuildStarts()
{
    memset(s_starts, 0, BitmapWords() * sizeof(*s_starts));
    memset(s_allocated, 0, BitmapWords() * sizeof(*s_allocated));

    NANOCLR_FOREACH_NODE(CLR_RT_HeapCluster, hc, g_CLR_RT_ExecutionEngine.m_heap)
    {
        for (CLR_RT_HeapBlock_Node *ptr = hc->m_payloadStart; ptr < hc->m_payloadEnd; ptr += ptr->DataSize())
        {
            if (IsObjectBlock(ptr))
            {
                SetBit(s_starts, (size_t)SlotIndex(ptr));
            }
        }
    }
    NANOCLR_FOREACH_NODE_END();
}

// A size-1 value slot standing alone in the heap walk. These are members of a run extracted in one allocation and
// restamped slot by slot (assembly static fields), or the tail of an object whose header was shrunk afterwards
// (SetReflection on a two-block MethodInfo, a known upstream bug). Nothing references them as an object.
bool IsOrphanSlot(const CLR_RT_HeapBlock *block)
{
    NanoCLRDataType dt = block->DataType();

    return block->DataSize() == 1 &&
           (dt < DATATYPE_STRING || dt == DATATYPE_OBJECT || dt == DATATYPE_BYREF || dt == DATATYPE_ARRAY_BYREF);
}

// Stress mode: compares the object-start bitmap with a heap walk.
void VerifyStarts(CLR_UINT32 &missing, CLR_UINT32 &orphans, CLR_UINT32 &stale, CLR_UINT32 &interior)
{
    missing = orphans = stale = interior = 0;

    NANOCLR_FOREACH_NODE(CLR_RT_HeapCluster, hc, g_CLR_RT_ExecutionEngine.m_heap)
    {
        for (CLR_RT_HeapBlock_Node *ptr = hc->m_payloadStart; ptr < hc->m_payloadEnd; ptr += ptr->DataSize())
        {
            size_t i = (size_t)SlotIndex(ptr);
            bool expected = IsObjectBlock(ptr);
            bool actual = TestBit(s_starts, i);

            if (expected && !actual && IsOrphanSlot(ptr))
            {
                orphans++;
            }
            else if (expected != actual)
            {
                (expected ? missing : stale)++;

                if (missing + stale <= 5)
                {
                    CLR_Debug::Printf(
                        "[GCI] bitmap %s: slot=%u type=0x%02x flags=0x%02x size=%u\r\n",
                        expected ? "missing" : "stale",
                        (unsigned)i,
                        (unsigned)ptr->DataType(),
                        (unsigned)ptr->DataFlags(),
                        (unsigned)ptr->DataSize());
                }
            }

            for (size_t j = i + 1; j < i + ptr->DataSize(); j++)
            {
                if (TestBit(s_starts, j))
                {
                    interior++;
                }
            }
        }
    }
    NANOCLR_FOREACH_NODE_END();
}

bool EnvFlag(const char *name)
{
    const char *value = getenv(name);

    return value != nullptr && value[0] != '\0' && strcmp(value, "0") != 0;
}

// Calls f(obj, slot) for every object block in the heap walk.
template <typename F> void ForEachObject(F f)
{
    NANOCLR_FOREACH_NODE(CLR_RT_HeapCluster, hc, g_CLR_RT_ExecutionEngine.m_heap)
    {
        for (CLR_RT_HeapBlock_Node *ptr = hc->m_payloadStart; ptr < hc->m_payloadEnd; ptr += ptr->DataSize())
        {
            if (IsObjectBlock(ptr))
            {
                f(ptr, (size_t)SlotIndex(ptr));
            }
        }
    }
    NANOCLR_FOREACH_NODE_END();
}

// Prints one object: slot, data type, flags, size and, for classes, value types and arrays, the managed type.
void ReportObject(const char *kind, CLR_RT_HeapBlock *obj, size_t slot)
{
    char name[256] = "";
    char *sz = name;
    size_t cb = MAXSTRLEN(name);
    NanoCLRDataType dt = obj->DataType();

    if ((dt == DATATYPE_CLASS || dt == DATATYPE_VALUETYPE) && obj->IsAGenericInstance())
    {
        g_CLR_RT_TypeSystem.BuildTypeName(obj->ObjectGenericType(), sz, cb, 0);
    }
    else if (dt == DATATYPE_CLASS || dt == DATATYPE_VALUETYPE)
    {
        g_CLR_RT_TypeSystem.BuildTypeName(obj->ObjectCls(), sz, cb);
    }
    else if (dt == DATATYPE_SZARRAY)
    {
        CLR_RT_TypeDef_Instance element{};
        CLR_UINT32 levels;

        if (element.InitializeFromReflection(((CLR_RT_HeapBlock_Array *)obj)->ReflectionData(), &levels))
        {
            g_CLR_RT_TypeSystem.BuildTypeName(element, sz, cb, CLR_RT_TypeSystem::TYPENAME_FLAGS_FULL, levels);
        }
    }

    name[MAXSTRLEN(name)] = 0;

    CLR_Debug::Printf(
        "[GCV] %s: slot=%u type=0x%02x flags=0x%02x size=%u %s\r\n",
        kind,
        (unsigned)slot,
        (unsigned)dt,
        (unsigned)obj->DataFlags(),
        (unsigned)obj->DataSize(),
        name);
}

// Verify mode: checks the current mark against a second stop-the-world mark. Runs right after Mark().
void VerifyMark()
{
    CLR_UINT32 objects = 0;
    CLR_UINT32 marked = 0;
    CLR_UINT32 injected = 0;
    CLR_UINT32 misses = 0;
    CLR_UINT32 floating = 0;
    CLR_UINT32 droppedDead = 0;

    memset(s_snapshot, 0, BitmapWords() * sizeof(*s_snapshot));

    if (s_dropped)
    {
        memset(s_dropped, 0, BitmapWords() * sizeof(*s_dropped));
    }

    // Snapshot the mark under test, then unmark it. Event blocks are left alone: they are permanently HB_Alive, the
    // marker never traces through them, and unmarking one would get it freed by this collection's sweep.
    ForEachObject([&](CLR_RT_HeapBlock *obj, size_t i) {
        objects++;

        if (obj->IsAlive())
        {
            marked++;

            if (s_dropped && marked % s_verifyInject == 0)
            {
                injected++;
                SetBit(s_dropped, i);
            }
            else
            {
                SetBit(s_snapshot, i);
            }

            obj->MarkDead();
        }
    });

    // Mark again from the roots without the finalizer pass. The finalizer pass has already run (in step 3, inside the
    // Mark() under test; the incremental final pause must also run it before verifying), so this collection's dead
    // finalizable objects are in m_finalizersPending, which Mark() marks as roots. Running the pass again would
    // queue for finalization any object the mark under test kept but this mark failed to reach.
    CLR_RT_DblLinkedList &finalizersAlive = g_CLR_RT_ExecutionEngine.m_finalizersAlive;
    CLR_RT_DblLinkedList aside;
    aside.DblLinkedList_Initialize();

    while (CLR_RT_HeapBlock_Node *fin = finalizersAlive.ExtractFirstNode())
    {
        aside.LinkAtBack(fin);
    }

    g_CLR_RT_GarbageCollector.Mark();

    while (CLR_RT_HeapBlock_Node *fin = aside.ExtractFirstNode())
    {
        finalizersAlive.LinkAtBack(fin);
    }

    // Compare, keeping the union of both marks.
    ForEachObject([&](CLR_RT_HeapBlock *obj, size_t i) {
        bool inSnapshot = TestBit(s_snapshot, i);

        if (obj->IsAlive() && !inSnapshot)
        {
            if (misses++ < c_verifyReportedMisses)
            {
                ReportObject("miss", obj, i);
            }
        }
        else if (!obj->IsAlive() && inSnapshot)
        {
            obj->MarkAlive();

            if (floating++ < c_verifyReportedFloating)
            {
                ReportObject("floating", obj, i);
            }

            // The sweep keeps a weak delegate list's delegates alive only for lists the mark linked as reachable.
            if (obj->DataType() == DATATYPE_DELEGATELIST_HEAD &&
                (((CLR_RT_HeapBlock_Delegate_List *)obj)->m_flags & CLR_RT_HeapBlock_Delegate_List::c_Weak))
            {
                obj->ClearData();
                g_CLR_RT_GarbageCollector.m_weakDelegates_Reachable.LinkAtBack((CLR_RT_HeapBlock_Node *)obj);
            }
        }
        else if (!obj->IsAlive() && s_dropped && TestBit(s_dropped, i))
        {
            droppedDead++;
        }
    });

    CLR_Debug::Printf(
        "[GCV] run=%u objects=%u marked=%u injected=%u misses=%u dropped_dead=%u floating=%u\r\n",
        (unsigned)g_CLR_RT_GarbageCollector.m_numberOfGarbageCollections,
        (unsigned)objects,
        (unsigned)marked,
        (unsigned)injected,
        (unsigned)misses,
        (unsigned)droppedDead,
        (unsigned)floating);
}

} // namespace

void GcIncremental_PreWrite(const CLR_RT_HeapBlock *slot)
{
    s_stats.preWrites++;

    if (InHeap(slot))
    {
        ShadeSlot(slot);
    }
}

void GcIncremental_ShadeBytes(const void *dst, size_t bytes)
{
    // Steps in block-sized strides from dst rather than snapping to the object grid: element storage sits at
    // &array[1], which is not grid-aligned when sizeof(CLR_RT_HeapBlock_Array) is not a multiple of the block size.
    s_stats.shadeBytes++;

    auto *slot = (const CLR_RT_HeapBlock *)dst;
    const uintptr_t heapEnd = (uintptr_t)(s_CLR_RT_Heap.location + s_CLR_RT_Heap.size);

    for (size_t n = (bytes + c_slotSize - 1) / c_slotSize; n > 0 && InHeap(slot) && (uintptr_t)&slot[1] <= heapEnd;
         n--, slot++)
    {
        ShadeSlot(slot);
    }
}

bool GcIncremental_IsAllocatedThisCycle(const CLR_RT_HeapBlock *block)
{
    ptrdiff_t i = s_valid ? SlotIndex(block) : -1;

    return i >= 0 && TestBit(s_allocated, (size_t)i);
}

void GcIncremental_ShadeObject(const CLR_RT_HeapBlock *obj)
{
    s_stats.weakShades++;
    Shade(obj);
}

// Free memory carries no bits (every path that frees clears its run; heap init and compaction rebuild), so a fresh
// block only needs its start bit. Stress mode checks this on every allocation (alloc_dirty) as well as at every full
// collection (bitmap_stale, bitmap_interior).
void GcIncremental_OnAllocate(const CLR_RT_HeapBlock *block, CLR_UINT32 flags)
{
    const bool isEvent = (flags & CLR_RT_HeapBlock::HB_Event) != 0;

    if (!s_valid || (isEvent && !s_stress))
    {
        return;
    }

    ptrdiff_t i = SlotIndex(block);

    if (i < 0)
    {
        return;
    }

    if (s_stress)
    {
        for (size_t j = (size_t)i; j < (size_t)i + block->DataSize(); j++)
        {
            if (TestBit(s_starts, j) || TestBit(s_allocated, j))
            {
                s_stats.dirtyAllocs++;
                break;
            }
        }
    }

    if (isEvent)
    {
        return;
    }

    SetBit(s_starts, (size_t)i);

    if (g_GcIncremental_BarrierActive)
    {
        SetBit(s_allocated, (size_t)i);
    }
}

void GcIncremental_OnFree(const CLR_RT_HeapBlock *block, CLR_UINT32 length)
{
    ptrdiff_t i = s_valid ? SlotIndex(block) : -1;

    if (i >= 0)
    {
        ClearBits(s_starts, (size_t)i, length);
        ClearBits(s_allocated, (size_t)i, length);
    }
}

void GcIncremental_OnHeapInitialized()
{
    NATIVE_PROFILE_CLR_CORE();

    s_valid = false;
    g_GcIncremental_BarrierActive = false;

    if (s_starts)
    {
        platform_free(s_starts);
    }

    if (s_allocated)
    {
        platform_free(s_allocated);
    }

    if (s_grey)
    {
        platform_free(s_grey);
    }

    if (s_snapshot)
    {
        platform_free(s_snapshot);
    }

    if (s_dropped)
    {
        platform_free(s_dropped);
    }

    s_verify = false;
    s_verifyInject = 0;
    s_starts = s_allocated = s_snapshot = s_dropped = nullptr;
    s_grey = nullptr;
    s_greyCount = s_greyCapacity = 0;
    s_stats = {};

    auto *first = (CLR_RT_HeapCluster *)g_CLR_RT_ExecutionEngine.m_heap.FirstValidNode();

    if (first == nullptr)
    {
        return;
    }

    s_base = (uintptr_t)first->m_payloadStart;
    s_end = (uintptr_t)(s_CLR_RT_Heap.location + s_CLR_RT_Heap.size);
    s_slots = (s_end - s_base) / c_slotSize;

    // Clusters are carved back to back in multiples of the block size, so every payload shares slot 0's grid.
    NANOCLR_FOREACH_NODE(CLR_RT_HeapCluster, hc, g_CLR_RT_ExecutionEngine.m_heap)
    {
        if (((uintptr_t)hc->m_payloadStart - s_base) % c_slotSize != 0)
        {
            CLR_Debug::Printf("[GCI] heap clusters are not on one block grid; incremental GC disabled\r\n");
            return;
        }
    }
    NANOCLR_FOREACH_NODE_END();

    s_starts = (CLR_UINT32 *)platform_malloc(BitmapWords() * sizeof(*s_starts));
    s_allocated = (CLR_UINT32 *)platform_malloc(BitmapWords() * sizeof(*s_allocated));

    if (s_starts == nullptr || s_allocated == nullptr)
    {
        CLR_Debug::Printf("[GCI] no host memory for the heap bitmaps; incremental GC disabled\r\n");
        return;
    }

    s_valid = true;
    RebuildStarts();

    s_stress = EnvFlag("NANOCLR_GC_BARRIER_STRESS");

    if (s_stress)
    {
        CLR_Debug::Printf("[GCI] barrier stress on: slots=%u\r\n", (unsigned)s_slots);
        g_GcIncremental_BarrierActive = true;
    }

    if (EnvFlag("NANOCLR_GC_VERIFY"))
    {
        const char *inject = getenv("NANOCLR_GC_VERIFY_INJECT");

        s_snapshot = (CLR_UINT32 *)platform_malloc(BitmapWords() * sizeof(*s_snapshot));
        s_verifyInject = inject ? (CLR_UINT32)strtoul(inject, nullptr, 10) : 0;

        if (s_verifyInject != 0)
        {
            s_dropped = (CLR_UINT32 *)platform_malloc(BitmapWords() * sizeof(*s_dropped));
        }

        s_verify = s_snapshot != nullptr && (s_verifyInject == 0 || s_dropped != nullptr);

        CLR_Debug::Printf(
            s_verify ? "[GCV] verify on: inject=%u\r\n" : "[GCV] no host memory for the verify bitmaps; verify off\r\n",
            (unsigned)s_verifyInject);
    }
}

void GcIncremental_OnMarked()
{
    NATIVE_PROFILE_CLR_CORE();

    if (s_valid && s_verify)
    {
        VerifyMark();
    }
}

void GcIncremental_OnHeapCompacted()
{
    if (s_valid)
    {
        RebuildStarts();
    }
}

void GcIncremental_OnFullMark()
{
    NATIVE_PROFILE_CLR_CORE();

    if (!s_valid)
    {
        return;
    }

    if (s_stress)
    {
        CLR_UINT32 missing;
        CLR_UINT32 orphans;
        CLR_UINT32 stale;
        CLR_UINT32 interior;

        VerifyStarts(missing, orphans, stale, interior);

        CLR_Debug::Printf(
            "[GCI] full-mark run=%u prewrite=%u shade_bytes=%u weak=%u pushed=%u black=%u rejected=%u "
            "grey_dropped=%u alloc_dirty=%u bitmap_missing=%u bitmap_orphans=%u bitmap_stale=%u bitmap_interior=%u\r\n",
            (unsigned)g_CLR_RT_GarbageCollector.m_numberOfGarbageCollections,
            (unsigned)s_stats.preWrites,
            (unsigned)s_stats.shadeBytes,
            (unsigned)s_stats.weakShades,
            (unsigned)s_stats.pushed,
            (unsigned)s_stats.alreadyBlack,
            (unsigned)s_stats.rejected,
            (unsigned)s_stats.greyDropped,
            (unsigned)s_stats.dirtyAllocs,
            (unsigned)missing,
            (unsigned)orphans,
            (unsigned)stale,
            (unsigned)interior);
    }

    // No incremental cycle exists yet (task 0002 step 4): a full collection simply discards the barrier's work.
    s_greyCount = 0;
    s_stats = {};
    memset(s_allocated, 0, BitmapWords() * sizeof(*s_allocated));
}

#endif // NANOCLR_GC_INCREMENTAL
