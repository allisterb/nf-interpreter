//
// Copyright (c) .NET Foundation and Contributors
// See LICENSE file in the project root for full license information.
//

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace
{
    constexpr size_t c_DefaultHeapSizeBytes = 10 * 1024 * 1024;
    constexpr size_t c_MinHeapSizeBytes = 1024 * 1024;
    constexpr size_t c_MaxHeapSizeBytes = 1024 * 1024 * 1024;

    unsigned char *g_MemoryStart = nullptr;
    unsigned char *g_CustomHeapStart = nullptr;
    std::mutex g_DefaultHeapMutex;
    std::mutex g_CustomHeapMutex;

    // Managed heap size: NANOCLR_HEAP_SIZE in decimal bytes (1 MiB to 1 GiB), else the default.
    size_t DefaultHeapSizeBytes()
    {
        const char *text = std::getenv("NANOCLR_HEAP_SIZE");

        if (text == nullptr || *text == '\0')
        {
            return c_DefaultHeapSizeBytes;
        }

        char *end = nullptr;
        errno = 0;
        unsigned long long value = std::strtoull(text, &end, 10);

        if (*text < '0' || *text > '9' || errno != 0 || *end != '\0' || value < c_MinHeapSizeBytes ||
            value > c_MaxHeapSizeBytes)
        {
            std::fprintf(
                stderr,
                "warning: ignoring NANOCLR_HEAP_SIZE='%s' (expected decimal bytes, %zu to %zu); using %zu\n",
                text,
                c_MinHeapSizeBytes,
                c_MaxHeapSizeBytes,
                c_DefaultHeapSizeBytes);
            return c_DefaultHeapSizeBytes;
        }

        return static_cast<size_t>(value);
    }
} // namespace

void HeapLocation(unsigned char *&BaseAddress, unsigned int &SizeInBytes)
{
    std::lock_guard<std::mutex> lock(g_DefaultHeapMutex);
    static const size_t heapSizeBytes = DefaultHeapSizeBytes();

    if (g_MemoryStart == nullptr)
    {
        // TODO: evaluate mmap() if we need stronger parity with win32 VirtualAlloc behavior.
        g_MemoryStart = static_cast<unsigned char *>(std::malloc(heapSizeBytes));

        if (g_MemoryStart != nullptr)
        {
            std::memset(g_MemoryStart, 0xEA, heapSizeBytes);
        }
        else
        {
            std::fprintf(
                stderr,
                "error: cannot allocate a %zu-byte managed heap (NANOCLR_HEAP_SIZE)\n",
                heapSizeBytes);
        }
    }

    BaseAddress = g_MemoryStart;
    SizeInBytes = (g_MemoryStart != nullptr) ? static_cast<unsigned int>(heapSizeBytes) : 0;
}

void CustomHeapLocation(unsigned char *&BaseAddress, unsigned int &SizeInBytes)
{
    std::lock_guard<std::mutex> lock(g_CustomHeapMutex);

    if (g_CustomHeapStart == nullptr)
    {
        // TODO: revisit custom heap ownership model when CLR runtime is wired.
        g_CustomHeapStart = static_cast<unsigned char *>(std::malloc(c_DefaultHeapSizeBytes));

        if (g_CustomHeapStart != nullptr)
        {
            std::memset(g_CustomHeapStart, 0xEA, c_DefaultHeapSizeBytes);
        }
    }

    BaseAddress = g_CustomHeapStart;
    SizeInBytes = (g_CustomHeapStart != nullptr) ? static_cast<unsigned int>(c_DefaultHeapSizeBytes) : 0;
}
