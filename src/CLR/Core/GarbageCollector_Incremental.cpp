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
// is kept (relinking restored weak delegate lists), so a miss never frees a live object. The mark under test is the
// incremental cycle's when one just finished, else the stop-the-world mark itself; NANOCLR_GC_VERIFY_INJECT=N drops
// every Nth marked object from the snapshot to test the detector. A dropped object that is still reachable comes back
// as a miss; one the second mark does not reach (an unreachable block that was already HB_Alive) is counted as
// dropped_dead, so misses + dropped_dead == injected on every collection.
//
// Incremental cycles (NANOCLR_GC_INCREMENTAL=1). Once NANOCLR_GC_INCREMENTAL_TRIGGER percent of the heap (default 25)
// has been allocated since the last collection, the next quantum boundary takes the initial pause: the roots of
// Mark() are snapshotted onto the grey stack and the barrier turns on. Each later quantum boundary scans up to
// NANOCLR_GC_INCREMENTAL_SLICE slots (default 4096) of grey objects. When the grey stack is empty, a full collection
// runs, and its Mark() is replaced by the final pause: drain, finalizer pass, then transfer of the cycle's mark bitmap
// (plus objects allocated during the cycle) to HB_Alive for MarkWeak and the sweep. A collection requested mid-cycle
// finishes the cycle the same way. Weak delegate lists are traced as strong during a cycle, so while cycles keep
// running their targets are collected only by a collection that runs without one. Compaction aborts a cycle, as does a
// lost grey push (the final pause then runs Mark()). Each finished cycle prints one "[GCC]" line.

#include "Core.h"

#if defined(NANOCLR_GC_INCREMENTAL)

#include <stdlib.h>
#include <string.h>
#include <time.h>

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

// One incremental cycle, reported on the "[GCC]" line when it ends.
struct GcIncrementalCycle
{
    uint64_t startNs;
    uint64_t initialNs;  // initial pause: root snapshot
    uint64_t sliceMaxNs; // longest mark slice
    uint64_t finalNs;    // final pause, mark part: drain, finalizer pass, transfer (sweep not included)
    CLR_UINT32 slices;
    CLR_UINT32 rootBlocks;
    CLR_UINT32 scannedObjects;
    CLR_UINT32 scannedSlots;
    CLR_UINT32 finalScannedObjects; // objects still grey at the final pause
    CLR_UINT32 allocatedBlack;
    CLR_UINT32 greyMax;
};

const size_t c_slotSize = sizeof(CLR_RT_HeapBlock);
const size_t c_bitsPerWord = 32;
const size_t c_stressGreyCap = 65536;
const size_t c_arrayChunk = 1024;              // reference arrays longer than this are scanned across slices
const CLR_UINT32 c_verifyReportedMisses = 5;   // misses printed in detail per collection
const CLR_UINT32 c_verifyReportedFloating = 3; // floating objects printed in detail per collection

bool s_valid;
bool s_stress;
bool s_verify;
CLR_UINT32 s_verifyInject; // drop every Nth marked object from the verify snapshot; 0 = never
CLR_UINT32 *s_snapshot;    // verify mode: objects the mark under test marked
CLR_UINT32 *s_dropped;     // verify mode with injection: marked objects dropped from the snapshot
// Verify mode: m_finalizersAlive while the re-mark runs. Moved with NANOCLR_FOREACH_NODE, as Mark() moves finalizers,
// never with an ExtractFirstNode() loop: Head() is a pseudo-node 8 bytes before the list, and GCC assumes a store
// through it cannot change m_first, so a loop whose inlined body re-reads the head never sees the update and spins (at
// any -O level for a list standing alone, at -O3 with strict aliasing for a member list). FOREACH reads the head once.
CLR_RT_DblLinkedList s_finalizersAside;
uintptr_t s_base; // address of slot 0: the first cluster's payload start
uintptr_t s_end;
size_t s_slots;
CLR_UINT32 *s_starts;    // object-start bitmap
CLR_UINT32 *s_allocated; // allocated-this-cycle bitmap
CLR_UINT32 *s_marked;    // incremental cycle: shaded (grey or black) objects and marked root slots
const CLR_RT_HeapBlock **s_grey;
size_t s_greyCount;
size_t s_greyCapacity;
const CLR_RT_HeapBlock *s_partialArray; // reference array being scanned in chunks, or null
size_t s_partialNext;                   // next element of s_partialArray to scan
GcIncrementalStats s_stats;

bool s_incremental;        // NANOCLR_GC_INCREMENTAL=1: run incremental cycles
bool s_cycleActive;        // between the initial and the final pause
bool s_greyOverflow;       // a grey push was lost during the cycle: the final pause falls back to Mark()
size_t s_triggerSlots;     // start a cycle once this many slots were allocated since the last collection
size_t s_sliceSlots;       // marking budget per quantum, in slots scanned
size_t s_allocatedSinceGc; // object slots allocated since the last collection
CLR_UINT32 s_cycleCount;
GcIncrementalCycle s_cycle;

uint64_t NowNs()
{
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);

    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

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
        auto *grey = (const CLR_RT_HeapBlock **)(s_stress && !s_cycleActive && capacity > c_stressGreyCap
                                                     ? nullptr
                                                     : platform_malloc(capacity * sizeof(*s_grey)));

        if (grey == nullptr)
        {
            s_stats.greyDropped++;
            s_greyOverflow = s_greyOverflow || s_cycleActive;
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

    if (s_greyCount > s_cycle.greyMax)
    {
        s_cycle.greyMax = (CLR_UINT32)s_greyCount;
    }
}

// Slot index of obj if it is the start of a live object block, else -1.
ptrdiff_t ObjectIndex(const CLR_RT_HeapBlock *obj)
{
    ptrdiff_t i = s_valid ? SlotIndex(obj) : -1;

    return i >= 0 && TestBit(s_starts, (size_t)i) && IsObjectBlock(obj) ? i : -1;
}

// Greys obj if it is an object not yet shaded or allocated this cycle. During a cycle the mark bit is set on push, so
// an object is pushed at most once; in barrier stress mode without a cycle nothing is marked.
void Shade(const CLR_RT_HeapBlock *obj)
{
    if (obj == nullptr)
    {
        return;
    }

    ptrdiff_t i = ObjectIndex(obj);

    if (i < 0)
    {
        s_stats.rejected++;
        return;
    }

    if (TestBit(s_marked, (size_t)i) || TestBit(s_allocated, (size_t)i))
    {
        s_stats.alreadyBlack++;
        return;
    }

    if (s_cycleActive)
    {
        SetBit(s_marked, (size_t)i);
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

// Marker version of ShadeSlot: like ComputeReachabilityGraph, a reference to a block that is not an object start (a
// BYREF into a field, a local or a static slot) is followed into that slot.
void TraceSlot(const CLR_RT_HeapBlock *slot, int depth = 0)
{
    const CLR_RT_HeapBlock *target;

    switch (slot->DataType())
    {
        case DATATYPE_OBJECT:
        case DATATYPE_BYREF:
            target = slot->Dereference();
            break;

        case DATATYPE_ARRAY_BYREF:
            target = (const CLR_RT_HeapBlock *)slot->Array();
            break;

        default:
            return;
    }

    if (target == nullptr)
    {
        return;
    }

    if (ObjectIndex(target) >= 0)
    {
        Shade(target);
    }
    else if (depth < 4)
    {
        TraceSlot(target, depth + 1);
    }
}

void TraceSlots(const CLR_RT_HeapBlock *slots, size_t count)
{
    for (size_t n = 0; n < count; n++)
    {
        TraceSlot(&slots[n]);
    }

    s_cycle.scannedSlots += (CLR_UINT32)count;
}

// Continues the reference array being scanned in chunks, for at most budget elements (at least one).
size_t ScanArrayChunk(size_t budget)
{
    auto *array = (CLR_RT_HeapBlock_Array *)s_partialArray;
    size_t remaining = array->m_numOfElements - s_partialNext;
    size_t n = remaining < budget ? remaining : (budget > 0 ? budget : 1);

    TraceSlots((CLR_RT_HeapBlock *)array->GetFirstElement() + s_partialNext, n);
    s_partialNext += n;

    if (s_partialNext >= array->m_numOfElements)
    {
        s_partialArray = nullptr;
    }

    return n;
}

// Blackens a grey object: shades every reference it holds. Mirrors ComputeReachabilityGraphForMultipleBlocks, except
// that weak delegate lists are traced like strong ones while a cycle runs: a delegate read out of a weak list into a
// root and stored into a black object would otherwise be lost (task 0002, audit finding C). A reference array longer
// than c_arrayChunk becomes the partial array, which Drain scans in chunks. Returns the slots scanned.
size_t ScanObject(const CLR_RT_HeapBlock *obj)
{
    auto *ptr = (CLR_RT_HeapBlock *)obj;
    size_t before = s_cycle.scannedSlots;

    s_cycle.scannedObjects++;

    switch (ptr->DataType())
    {
        case DATATYPE_CLASS:
        case DATATYPE_VALUETYPE:
            TraceSlots(ptr + 1, ptr->DataSize() - 1);
            break;

        case DATATYPE_SZARRAY:
        {
            auto *array = (CLR_RT_HeapBlock_Array *)ptr;

            if (array->IsStoragePointer())
            {
                TraceSlots(array->StorageOwner(), 1);
            }
            else if (array->m_fReference && array->m_numOfElements > c_arrayChunk)
            {
                s_partialArray = array;
                s_partialNext = 0;
            }
            else if (array->m_fReference)
            {
                TraceSlots((CLR_RT_HeapBlock *)array->GetFirstElement(), array->m_numOfElements);
            }
        }
        break;

        case DATATYPE_DELEGATE_HEAD:
            TraceSlots(&((CLR_RT_HeapBlock_Delegate *)ptr)->m_object, 1);
            break;

        case DATATYPE_DELEGATELIST_HEAD:
        {
            auto *list = (CLR_RT_HeapBlock_Delegate_List *)ptr;

            TraceSlots(list->GetDelegates(), list->m_length);
        }
        break;

        case DATATYPE_OBJECT:
        case DATATYPE_BYREF:
        case DATATYPE_ARRAY_BYREF:
            // A slot standing alone in the heap. On posix (no AppDomains) static fields live inside the assembly's
            // event block, so this is reached only by stray stand-alone slots; it mirrors the upstream marker.
            TraceSlots(ptr, 1);
            break;

        default:
            break;
    }

    return s_cycle.scannedSlots - before + 1;
}

// Scans grey objects (and the partial array first) until both are exhausted or about budget slots were scanned.
// Returns true when no grey work is left.
bool Drain(size_t budget)
{
    size_t work = 0;

    while (work < budget)
    {
        if (s_partialArray != nullptr)
        {
            work += ScanArrayChunk(budget - work);
            continue;
        }

        if (s_greyCount == 0)
        {
            break;
        }

        const CLR_RT_HeapBlock *obj = s_grey[--s_greyCount];

        // Defensive: nothing frees heap memory between the pauses (only the sweep does, and compaction aborts).
        if (ObjectIndex(obj) >= 0)
        {
            work += ScanObject(obj);
        }
    }

    return s_greyCount == 0 && s_partialArray == nullptr;
}

// Root snapshot, installed as the collector's marking functions during the initial pause. A root block that is an
// object start is greyed. Anything else is a slot (frame arguments, locals and eval stack, static fields inside the
// assembly's event block, a ProtectFromGC'd slot), whose referent is shaded; when the slot happens to lie on the block
// grid its mark bit is set too, which only matters for a slot that stands alone in the heap.
void ShadeRootBlock(CLR_RT_HeapBlock *block)
{
    s_cycle.rootBlocks++;

    if (ObjectIndex(block) >= 0)
    {
        Shade(block);
        return;
    }

    ptrdiff_t i = SlotIndex(block);

    if (i >= 0)
    {
        SetBit(s_marked, (size_t)i);
    }

    TraceSlot(block);
}

bool ShadeRootSingle(CLR_RT_HeapBlock **ptr)
{
    if (*ptr != nullptr)
    {
        ShadeRootBlock(*ptr);
    }

    return true;
}

bool ShadeRootMultiple(CLR_RT_HeapBlock *lst, CLR_UINT32 num)
{
    for (; num > 0; num--, lst++)
    {
        ShadeRootBlock(lst);
    }

    return true;
}

// The root list of CLR_RT_GarbageCollector::Mark(), with the same build conditions. Keep the two in step; verify mode
// reports any object a missing root would lose.
void ShadeRoots()
{
    CLR_RT_GarbageCollector &gc = g_CLR_RT_GarbageCollector;
    CLR_RT_GarbageCollector::MarkSingleFtn single = gc.m_funcSingleBlock;
    CLR_RT_GarbageCollector::MarkMultipleFtn multiple = gc.m_funcMultipleBlocks;

    gc.m_funcSingleBlock = ShadeRootSingle;
    gc.m_funcMultipleBlocks = ShadeRootMultiple;

    CLR_RT_ProtectFromGC::InvokeAll();
    g_CLR_HW_Hardware.PrepareForGC();

    NANOCLR_FOREACH_NODE(CLR_RT_HeapBlock_Finalizer, fin, g_CLR_RT_ExecutionEngine.m_finalizersPending)
    {
        gc.CheckSingleBlock(&fin->m_object);
    }
    NANOCLR_FOREACH_NODE_END();

#if defined(NANOCLR_APPDOMAINS)
    gc.AppDomain_Mark();
#endif

    gc.Assembly_Mark();

    gc.Thread_Mark(g_CLR_RT_ExecutionEngine.m_threadsReady);
    gc.Thread_Mark(g_CLR_RT_ExecutionEngine.m_threadsWaiting);

#if !defined(NANOCLR_APPDOMAINS)
    gc.CheckSingleBlock_Force(g_CLR_RT_ExecutionEngine.m_globalLock);
    gc.CheckSingleBlock_Force(g_CLR_RT_ExecutionEngine.m_outOfMemoryException);
#endif

    gc.CheckSingleBlock_Force(g_CLR_RT_ExecutionEngine.m_currentUICulture);

#if defined(NANOCLR_ENABLE_SOURCELEVELDEBUGGING)
    gc.CheckSingleBlock_Force(g_CLR_RT_ExecutionEngine.m_scratchPadArray);
#endif

    gc.m_funcSingleBlock = single;
    gc.m_funcMultipleBlocks = multiple;
}

void ClearCycleBitmaps()
{
    memset(s_marked, 0, BitmapWords() * sizeof(*s_marked));
    memset(s_allocated, 0, BitmapWords() * sizeof(*s_allocated));
}

// Initial pause: snapshot the roots and turn the barrier on.
void StartCycle()
{
    s_cycle = {};
    s_cycle.startNs = NowNs();
    s_cycleCount++;

    ClearCycleBitmaps();
    s_greyCount = 0;
    s_partialArray = nullptr;
    s_greyOverflow = false;
    s_cycleActive = true;
    g_GcIncremental_BarrierActive = true;

    ShadeRoots();

    s_cycle.initialNs = NowNs() - s_cycle.startNs;
}

void EndCycle()
{
    s_cycleActive = false;
    g_GcIncremental_BarrierActive = s_stress;
    s_greyCount = 0;
    s_partialArray = nullptr;
    ClearCycleBitmaps();
}

void AbortCycle(const char *reason)
{
    CLR_Debug::Printf(
        "[GCC] cycle=%u aborted: %s after %u slices\r\n",
        (unsigned)s_cycleCount,
        reason,
        (unsigned)s_cycle.slices);

    EndCycle();
}

void RebuildStarts()
{
    memset(s_starts, 0, BitmapWords() * sizeof(*s_starts));
    memset(s_allocated, 0, BitmapWords() * sizeof(*s_allocated));
    memset(s_marked, 0, BitmapWords() * sizeof(*s_marked));

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

// A size-1 value slot standing alone in the heap walk: on posix, the field slots left behind when SetReflection
// restamps an object header to one block (a known upstream bug). Nothing references them as an object.
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
    s_finalizersAside.DblLinkedList_Initialize();

    NANOCLR_FOREACH_NODE(CLR_RT_HeapBlock_Node, fin, finalizersAlive)
    {
        s_finalizersAside.LinkAtBack(fin);
    }
    NANOCLR_FOREACH_NODE_END();

    g_CLR_RT_GarbageCollector.Mark();

    NANOCLR_FOREACH_NODE(CLR_RT_HeapBlock_Node, fin, s_finalizersAside)
    {
        finalizersAlive.LinkAtBack(fin);
    }
    NANOCLR_FOREACH_NODE_END();

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
            if (TestBit(s_starts, j) || TestBit(s_allocated, j) || TestBit(s_marked, j))
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
    s_allocatedSinceGc += block->DataSize();

    if (g_GcIncremental_BarrierActive)
    {
        SetBit(s_allocated, (size_t)i);
        s_cycle.allocatedBlack++;
    }
}

void GcIncremental_OnFree(const CLR_RT_HeapBlock *block, CLR_UINT32 length)
{
    ptrdiff_t i = s_valid ? SlotIndex(block) : -1;

    if (i >= 0)
    {
        ClearBits(s_starts, (size_t)i, length);
        ClearBits(s_allocated, (size_t)i, length);
        ClearBits(s_marked, (size_t)i, length);
    }
}

void GcIncremental_OnHeapInitialized()
{
    NATIVE_PROFILE_CLR_CORE();

    s_valid = false;
    g_GcIncremental_BarrierActive = false;
    s_cycleActive = false;
    s_incremental = false;
    s_allocatedSinceGc = 0;
    s_cycleCount = 0;

    if (s_starts)
    {
        platform_free(s_starts);
    }

    if (s_allocated)
    {
        platform_free(s_allocated);
    }

    if (s_marked)
    {
        platform_free(s_marked);
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
    s_starts = s_allocated = s_marked = s_snapshot = s_dropped = nullptr;
    s_grey = nullptr;
    s_greyCount = s_greyCapacity = 0;
    s_partialArray = nullptr;
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
    s_marked = (CLR_UINT32 *)platform_malloc(BitmapWords() * sizeof(*s_marked));

    if (s_starts == nullptr || s_allocated == nullptr || s_marked == nullptr)
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

    s_incremental = EnvFlag("NANOCLR_GC_INCREMENTAL");

    if (s_incremental)
    {
        const char *trigger = getenv("NANOCLR_GC_INCREMENTAL_TRIGGER");
        const char *slice = getenv("NANOCLR_GC_INCREMENTAL_SLICE");
        unsigned long percent = trigger ? strtoul(trigger, nullptr, 10) : 25;

        percent = percent == 0 ? 25 : (percent > 100 ? 100 : percent);
        s_triggerSlots = s_slots * percent / 100;
        s_sliceSlots = slice && strtoul(slice, nullptr, 10) ? strtoul(slice, nullptr, 10) : 4096;

        CLR_Debug::Printf(
            "[GCC] incremental on: trigger=%u slots slice=%u slots\r\n",
            (unsigned)s_triggerSlots,
            (unsigned)s_sliceSlots);
    }
}

bool GcIncremental_FinishCycle()
{
    NATIVE_PROFILE_CLR_CORE();

    if (!s_cycleActive)
    {
        return false;
    }

    uint64_t start = NowNs();
    CLR_UINT32 scannedBefore = s_cycle.scannedObjects;

    Drain((size_t)-1);

    if (s_greyOverflow)
    {
        AbortCycle("grey stack overflow; full mark instead");
        return false;
    }

    // The finalizer pass of Mark(): an unmarked finalizable object is resurrected for its finalizer, and marked through
    // before the next entry is judged, so one only reachable from an earlier resurrected object stays alive-listed.
    NANOCLR_FOREACH_NODE(CLR_RT_HeapBlock_Finalizer, fin, g_CLR_RT_ExecutionEngine.m_finalizersAlive)
    {
        ptrdiff_t i = ObjectIndex(fin->m_object);

        if (i >= 0 && !TestBit(s_marked, (size_t)i) && !TestBit(s_allocated, (size_t)i))
        {
            Shade(fin->m_object);
            Drain((size_t)-1);
            g_CLR_RT_ExecutionEngine.m_finalizersPending.LinkAtBack(fin);
        }
    }
    NANOCLR_FOREACH_NODE_END();

    if (s_greyOverflow)
    {
        // The finalizer decisions above were made on a complete mark; Mark() treats the newly pending ones as roots.
        AbortCycle("grey stack overflow after the finalizer pass; full mark instead");
        return false;
    }

    s_cycle.finalScannedObjects = s_cycle.scannedObjects - scannedBefore;
    uint64_t transferStart = NowNs();

    // Transfer: the cycle's mark becomes HB_Alive on every non-event block, for MarkWeak and the sweep.
    ForEachObject([](CLR_RT_HeapBlock *obj, size_t i) {
        if (TestBit(s_marked, i) || TestBit(s_allocated, i))
        {
            obj->MarkAlive();
        }
        else
        {
            obj->MarkDead();
        }
    });

    // Weak delegate lists were traced as strong, so the sweep has no weak lists to process. Their targets die only in a
    // collection that runs without a cycle (Mark()).
    g_CLR_RT_GarbageCollector.m_weakDelegates_Reachable.DblLinkedList_Initialize();

    uint64_t end = NowNs();
    s_cycle.finalNs = end - start;

    CLR_Debug::Printf(
        "[GCC] cycle=%u run=%u slices=%u initial_us=%u slice_max_us=%u final_mark_us=%u transfer_us=%u total_ms=%u "
        "roots=%u scanned=%u slots=%u final_scanned=%u allocated_black=%u grey_max=%u\r\n",
        (unsigned)s_cycleCount,
        (unsigned)g_CLR_RT_GarbageCollector.m_numberOfGarbageCollections,
        (unsigned)s_cycle.slices,
        (unsigned)(s_cycle.initialNs / 1000),
        (unsigned)(s_cycle.sliceMaxNs / 1000),
        (unsigned)(s_cycle.finalNs / 1000),
        (unsigned)((end - transferStart) / 1000),
        (unsigned)((NowNs() - s_cycle.startNs) / 1000000),
        (unsigned)s_cycle.rootBlocks,
        (unsigned)s_cycle.scannedObjects,
        (unsigned)s_cycle.scannedSlots,
        (unsigned)s_cycle.finalScannedObjects,
        (unsigned)s_cycle.allocatedBlack,
        (unsigned)s_cycle.greyMax);

    EndCycle();

    return true;
}

void GcIncremental_OnQuantum()
{
    if (!s_valid || !s_incremental)
    {
        return;
    }

    if (!s_cycleActive)
    {
        // A pending compaction would abort the cycle as soon as ScheduleThreads returns.
        if (s_allocatedSinceGc >= s_triggerSlots && !CLR_EE_IS(Compaction_Pending))
        {
            StartCycle();
        }

        return;
    }

    if (s_greyOverflow)
    {
        // The cycle cannot finish soundly: abort it and collect stop-the-world now. That also resets the trigger, which
        // would otherwise start another cycle at the next quantum.
        AbortCycle("grey stack overflow; full collection instead");
        g_CLR_RT_ExecutionEngine.PerformGarbageCollection();
        return;
    }

    uint64_t start = NowNs();
    bool done = Drain(s_sliceSlots);
    uint64_t elapsed = NowNs() - start;

    s_cycle.slices++;

    if (elapsed > s_cycle.sliceMaxNs)
    {
        s_cycle.sliceMaxNs = elapsed;
    }

    if (done)
    {
        // The final pause: a full collection, which finishes the cycle in place of Mark().
        g_CLR_RT_ExecutionEngine.PerformGarbageCollection();
    }
}

void GcIncremental_OnCompaction()
{
    if (s_cycleActive)
    {
        AbortCycle("heap compaction");
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

    s_stats = {};
    s_allocatedSinceGc = 0;

    // With a cycle active, this collection is its final pause (GcIncremental_FinishCycle), which needs the grey stack
    // and the bitmaps. Otherwise any barrier work (stress mode) is simply discarded.
    if (!s_cycleActive)
    {
        s_greyCount = 0;
        memset(s_allocated, 0, BitmapWords() * sizeof(*s_allocated));
    }
}

#endif // NANOCLR_GC_INCREMENTAL
