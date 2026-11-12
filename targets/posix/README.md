# .NET nanoFramework nanoCLR - POSIX Host Target

This target provides a host build of nanoCLR for macOS and Linux at `targets/posix`. It can also be
cross-compiled for QNX 8.0 (x86_64, aarch64) without networking.

## Status

The target is fully functional as a managed-code execution host:

- Produces `nanoFramework.nanoCLR.dylib` (macOS) / `.so` (Linux) and a standalone application runner `nanoFramework.nanoCLR.test`, matching the architecture of the win32 virtual-device target. The `.test` suffix is part of the runner's name, not a separate test suite.
- The full CLR core (`src/CLR/Core/`), CoreLib (`src/CLR/CorLib/`), GC, type system, execution engine, and thread scheduler are compiled and linked.
- Managed PE assemblies are loaded from memory buffers and executed by the CLR.
- All native assemblies from the win32 target that are applicable to a software host are registered (see [Native Assembly Table](#native-assembly-table) below).
- CMake + Ninja builds are configured in GitHub Actions for macOS and Linux.

## Build

### macOS (Apple Silicon)

```bash
cmake -S targets/posix -B build/posix -G Ninja -DNANO_POSIX_ARCH=arm64
cmake --build build/posix
```

### macOS (Intel)

```bash
cmake -S targets/posix -B build/posix -G Ninja -DNANO_POSIX_ARCH=x86_64
cmake --build build/posix
```

### Linux

```bash
cmake -S targets/posix -B build/posix -G Ninja
cmake --build build/posix
```

### QNX 8.0 (cross-compile)

Requires the QNX SDP 8.0 environment (`QNX_HOST`, `QNX_TARGET`, e.g. from `qnxsdp-env.bat` /
`qnxsdp-env.sh`), CMake 3.24+ and Ninja. Use `toolchain-aarch64-qnx.cmake` for aarch64.

```bash
cmake -S targets/posix -B build/posix-qnx-x86_64 -G Ninja -DCMAKE_TOOLCHAIN_FILE=toolchain-x86_64-qnx.cmake
cmake --build build/posix-qnx-x86_64
```

Networking is not available on QNX (`NANO_POSIX_ENABLE_NETWORKING` is `OFF`). On the target the
runner needs `libc++.so.2` from the QNX image. The QNX runtime linker does not expand `$ORIGIN`, so
the runner carries no RPATH: point `LD_LIBRARY_PATH` at the deployed `lib/` directory.

Both macOS and Linux networking statically link the repository-pinned mbedTLS
(currently 3.6.7); no OpenSSL or system mbedTLS development/runtime packages are
required by the runtime. The first configure fetches its sources using Git.
For an existing checkout, pass `-DMBEDTLS_SOURCE=/absolute/path/to/mbedtls`.
macOS additionally links the system SystemConfiguration and CoreFoundation
frameworks. Xcode command-line tools and an accepted Xcode license are required.

Keep the generated `bin/`, `lib/` and `share/` directories together when deploying.
`share/nanoFramework/` contains third-party licenses. No CA bundle is shipped.

### Local networking validation (macOS and Linux)

Run a managed networking application with matching dependency PEs using the
standalone runner; see [Running Managed Applications](#running-managed-applications)
and the [managed loopback example](#managed-loopback-example-macos-linux-or-wsl).
This exercises the native backend, managed CLR scheduling and `System.Net`
marshalling together. There is no separate native networking test executable.

Use a separate build directory for each OS/architecture. On macOS the architecture
defaults to the host. Compare the application's interfaces and gateway with
`ifconfig` and `route -n get default` on macOS; compare DNS with `scutil --dns`.
VPN/split-DNS configurations can contain more servers than the two system-wide
IPv4 addresses exposed by the managed snapshot.

## Configuration Options

`NANO_POSIX_ARCH` *(macOS only)*
- Default: host architecture (`arm64` on Apple Silicon, `x86_64` on Intel)
- Accepted values: `arm64`, `x86_64`
- Has no effect on Linux (CMake uses the host architecture automatically).

`NANO_POSIX_ENABLE_SMOKE`
- Default: `ON`
- Builds the standalone command-line application runner, including its startup banner/version output.

`NANO_POSIX_ENABLE_NETWORKING`
- Default: `ON` on macOS and Linux, `OFF` elsewhere (e.g. QNX)
- Builds the host-OS sockets, DNS and TLS backend and registers `System.Net`. When `OFF`, mbedTLS is
  not fetched and the runner rejects `--ca-bundle`. Turning it on outside macOS and Linux is a
  configure error.

`NANO_POSIX_GC_TIMING`
- Default: `OFF`
- For measurement builds. Prints one line per garbage collection and per heap compaction to the
  debug output (stdout, or the callback set with `nanoCLR_SetDebugPrintCallback`), in order with
  `Debug.WriteLine` output. Durations are in nanoseconds, timed with `ClockCycles()` on QNX and
  `CLOCK_MONOTONIC` elsewhere:

  ```text
  [GCT] clock=ClockCycles ticks_per_sec=2495974759
  [GCT] gc run=12 total_ns=... prepare_ns=... mark_ns=... markweak_ns=... sweep_ns=... ratio_ns=... pressure_ns=... markslow_passes=0 markstack_chunks=1 markstack_entries=16384 markstack_failed=0 markstack_alloc_ns=... used_bytes=... heap_bytes=...
  [GCT] compact run=3 total_ns=... used_bytes=... heap_bytes=...
  ```

  - `run` is the collector's own counter. The native `System.GC.Run(true)`
    (`Run___STATIC__U4__BOOLEAN`) decrements it after collecting, so numbers can repeat.
  - `markslow_passes` counts full-heap rescans after the mark stack ran out of space.
  - `markstack_*` describe the overflow chunks beyond the 128 mark-stack entries on the C stack:
    chunks allocated, their entries (sentinels included), allocation attempts that failed or were
    refused by the host cap, and the time spent allocating.
  - `heap_bytes` is the sum of the heap clusters' payloads, a little less than the heap size.
  - On a `compact` line, `used_bytes` and `heap_bytes` are as of the last collection, which may be
    well before the compaction.
  - On QNX x86_64, `ClockCycles()` reads the TSC. If the CPUs' TSCs are not synchronised (possible
    under some hypervisors), a thread that moves between CPUs can record a wrapped, huge duration.
    Pin the process to one CPU if that happens.

`NANO_POSIX_GC_HOST_MARK_STACK`
- Default: `ON`
- When marking outgrows its 128-entry stack, take further stack chunks of 16384 entries (about
  256 KiB on 64-bit, 128 KiB on 32-bit) from `platform_malloc` instead of from the managed heap.
  Chunks are freed when the mark ends. Host memory held during one mark is capped at the managed
  heap's size; past that, marking falls back to full-heap rescans as before.
- Why: collections usually run because the managed heap is full. Carving mark-stack chunks out of
  it then degrades to many tiny allocations, or fails into repeated full-heap rescans. With deep
  object graphs (linked lists, trees) that took GC pauses from milliseconds to seconds. On QNX
  8.0 x86_64, allocation-triggered collections with a 6 MiB linked list live went from 0.65 s
  (10 MiB heap) and 2.25 s (40 MiB heap) to 5–9 ms (medians).
- `OFF` restores the upstream behaviour.

`NANO_POSIX_GC_INCREMENTAL`
- Default: `OFF`
- Experimental incremental, snapshot-at-the-beginning (SATB) collector. When `ON`, the build contains:
  - a deletion write barrier in the `CLR_RT_HeapBlock` setters and at raw heap copies;
  - keep-alive shading where weak pointers are read;
  - object-start, event-start, block-end, allocated-this-cycle and marked bitmaps in host memory,
    one bit per heap block each;
  - an incremental mark cycle, which is off unless `NANOCLR_GC_INCREMENTAL=1` is set;
  - free-memory accounting during the sweep. Every collection takes its free-byte count from the
    free runs the sweep rebuilds the free lists from, instead of walking every free-list node
    afterwards. Barrier stress and verify modes also do the walk and print
    `[GCI] free count mismatch ...` if the two differ.

  When `OFF`, every hook compiles to nothing.
- `NANOCLR_GC_INCREMENTAL=1` turns on incremental cycles.
  - **Start:** once `NANOCLR_GC_INCREMENTAL_TRIGGER` percent of the heap (1–100, default 25) has been
    allocated since the last collection, the next thread-quantum boundary takes the initial pause.
    The pause snapshots the roots and turns the barrier on.
  - **Slices:** each later quantum boundary scans about `NANOCLR_GC_INCREMENTAL_SLICE` slots of grey
    objects (default 4096). This is a soft target: an object is scanned whole, except that
    reference arrays longer than 1024 elements are scanned in chunks.
  - **Finish:** when no grey work is left, a full collection runs with the cycle's mark in place of
    the stop-the-world mark. The sweep is still stop-the-world.
  - **Bitmap sweep:** that collection rebuilds the free lists from the bitmaps instead of walking
    every block header. The bitmaps hold object starts, event-block starts and block ends, and the
    collection writes only the first header of each free run.
    - It sweeps one heap cluster (about 1.5 MiB) at a time, checking that cluster's bitmaps against
      themselves before writing any of its memory. A cluster whose bitmaps disagree falls back to
      the header walk on its own, printing `[GCS] self-check: ...` and
      `[GCS] run=... cluster at slot ...: ...; header sweep instead`. So does a cluster holding a
      block marked alive that is not an object start; if there are more than 8 of those, or one
      off the block grid, the whole collection uses the header walk.
    - `NANOCLR_GC_INCREMENTAL_SWEEP_FALLBACK=N` sends every Nth cluster the bitmap sweep would
      take to the header walk instead, to test the fallback. These are counted in
      `fallback_clusters` but not printed. The startup line shows the setting as `fallback_every`.
    - `NANOCLR_GC_INCREMENTAL_HEADER_SWEEP=1` always uses the header walk.
    - Collections without a cycle, and cycles under verify mode, always use the header walk.
    - Blocks alive only through a stale `HB_Alive`, such as the slots `SetReflection` cuts off an
      object, are freed one collection earlier than by the header walk.
    - With `NANOCLR_GC_BARRIER_STRESS=1`, every bitmap sweep is first compared with what the header
      walk would free, cluster by cluster. Any disagreement other than such blocks is printed
      (`[GCS] split: ...`, `kept-dead` or `freed-live`) and that cluster takes the header walk
      instead. `strays_freed`, `orphans_kept` and `alive_kept` count only the clusters the bitmap
      sweep kept; `free_blocks` counts the free blocks of every cluster. Each collection then prints:

      ```text
      [GCS] run=12 runs=... free_blocks=... dead_objects=... strays_freed=... orphans_kept=0 alive_kept=0 fallback_clusters=0
      ```

      `orphans_kept` and `alive_kept` should be 0. `orphans_kept` counts slots cut off an object by a
      truncation the bitmaps missed. `alive_kept` counts kept non-event blocks still carrying
      `HB_Alive`, which no bitmap sweep clears.
  - **Requested collections:** a collection requested mid-cycle, for example on allocation failure
    or `GC.Collect`, discards the cycle and marks stop-the-world. This gives the exact result: the
    cycle's snapshot would keep everything reachable at its start or allocated since. Such a cycle
    shows up as `aborted: collection requested mid-cycle`. If that happens often, cycles start too
    late or the slices are too small.
  - **GC-stress mode:** `NANOCLR_GC_INCREMENTAL_STRESS=N` also starts a cycle or runs one slice
    every N object allocations, from inside the allocator. This interleaves marking with native
    code as well as with IL. The final pause still waits for the next quantum boundary. Combine it
    with `NANOCLR_GC_VERIFY=1`.
    - The `[GCC]` line's `slices` and `slice_max_us` include slices run from the allocator.
    - A miss reported in this mode is not necessarily a barrier hole. It can also be an upstream
      native that holds an object only in a C++ local across an allocation, which
      `NANOCLR_FORCE_GC_BEFORE_EVERY_ALLOCATION` would expose too.
  - **Weak delegate lists** are traced as strong during a cycle. While cycles keep running, their
    targets are collected only by a collection that runs without a cycle.
  - **Aborts:** compaction aborts a cycle. A lost grey push (host memory exhausted) aborts it too,
    and a stop-the-world collection runs in its place.
  - With `NANOCLR_GC_INCREMENTAL_STATS=1`, each finished cycle prints one line. It is printed at the
    next quantum boundary rather than in the final pause, because a write to stdout is a message to
    the filesystem manager on QNX and cost about 60 µs of every pause. The running thread still pays
    that cost at the quantum boundary, so leave stats off where latency matters.
    - The line waits while every thread is blocked, and is lost if the program ends before another
      quantum. A soft reboot prints it first.
    - The startup line `[GCC] incremental on: ...` shows `stats=1`.
    - `[GCC] cycle=N aborted: ...` lines print whatever the setting, inside the collection that
      aborts the cycle. That collection is a stop-the-world one, which costs far more than the print.

    ```text
    [GCC] cycle=3 run=7 slices=2 initial_us=4 slice_max_us=... final_mark_us=... live_us=... sweep_us=... bitmap_sweep=1 total_ms=... roots=... scanned=... slots=... final_scanned=... allocated_black=... grey_max=... pre_us=... heap_us=... clear_us=... mask_us=... apply_us=... post_us=... free_runs=... dead_objects=... fallback_clusters=...
    ```

    - `final_mark_us` is the final pause's marking: draining what is still grey, the finalizer
      pass, and `live_us`, which publishes the live bitmap. Objects allocated during the cycle are
      marked as they are allocated, so `live_us` is close to 0.
    - `sweep_us` is the rest of the final pause: weak references and the sweep, which reads
      liveness from the live bitmap rather than from a heap walk that copies it into `HB_Alive`.
      With `NANOCLR_GC_VERIFY=1` it includes verify mode's second mark.
    - `sweep_us` splits into `pre_us` (verify mode's second mark, weak references and
      `RecoverEventsFromGC`), `heap_us` (the heap sweep) and `post_us` (clearing the cycle's
      bitmaps). For a bitmap sweep, `heap_us` splits further into `clear_us` (the scratch bitmap),
      `mask_us` (every cluster's free mask and its self-check, plus the shadow check under barrier
      stress) and `apply_us` (the rest: for each cluster, clearing the freed slots' start, event and
      end bits in one pass over its bitmap words, then writing the free runs, or the header walk for
      a cluster that fell back; plus the `[GCS]` lines), with the `free_runs` the bitmap sweep
      wrote, the `dead_objects` it freed, and the `fallback_clusters` that took the header walk.
      `clear_us`, `mask_us`, `apply_us`, `free_runs`, `dead_objects` and `fallback_clusters` are 0
      after a header walk. Each part is truncated to whole microseconds on its own, so the parts can
      add up to a microsecond or two less than the whole.
    - `total_ms` runs from the initial pause to the end of the sweep.
    - `bitmap_sweep` is 1 when the bitmap sweep ran, even if some clusters fell back, and 0 when
      the header walk did for the whole heap.
    - `final_scanned` counts the objects still grey at that point. It is 0 when the slices kept up.
- `NANOCLR_GC_BARRIER_STRESS=1` in the environment turns the barrier on for the whole run. Shades are
  validated and counted but never marked, and each collection checks the object-start bitmap against
  a heap walk and prints one line:

  ```text
  [GCI] full-mark run=12 prewrite=... shade_bytes=... weak=... pushed=... black=... rejected=... grey_dropped=... alloc_dirty=0 bitmap_missing=0 bitmap_orphans=... bitmap_stale=0 bitmap_interior=0 bitmap_blocks=0
  ```

  Any non-zero `alloc_dirty`, `bitmap_missing`, `bitmap_stale`, `bitmap_interior` or
  `bitmap_blocks` count is a bug: a heap path that allocates, frees or reshapes a block without
  updating the bitmaps. `bitmap_blocks` counts blocks whose event-start and end bits disagree with
  their header. `alloc_dirty` counts allocations
  (since the last collection) whose memory still carried a bit, which allocation relies on never
  happening. `bitmap_orphans` counts size-1 value slots standing alone in the heap walk. On posix
  these are the field slots left behind when `SetReflection` restamps an object header to one block.
  They are expected.
  The check walks the whole heap at the start of every collection, so do not combine stress mode
  with `NANO_POSIX_GC_TIMING` measurements.
- `NANOCLR_GC_VERIFY=1` turns on verify mode, which checks each mark against a second
  stop-the-world mark:
  1. It snapshots the marked objects and unmarks them.
  2. It marks again from the roots.
  3. It counts the objects the second mark finds live that the snapshot lacks (misses), and the
     reverse (floating).
  4. It keeps the union of both marks, so a miss never frees a live object.

  It prints up to five misses and three floating objects per collection, then one line:

  ```text
  [GCV] run=12 objects=... marked=... injected=0 misses=0 dropped_dead=0 floating=... stray_alive=0
  ```

  - After an incremental cycle, the mark under test is the cycle's. Otherwise it is the
    stop-the-world mark itself. `misses` must be 0 either way.
  - `stray_alive` must be 0. After a cycle, it counts blocks that carried `HB_Alive` outside the
    cycle's mark and hold something the marker traces (class and value-type fields, reference
    arrays, delegates, reference slots). Blocks that hold nothing traced are not counted: the
    stand-alone slots described below, and `System.Type` objects, which `Type.GetTypeFromHandle`
    fills by copying a whole slot, flags included. Verify after a cycle frees stand-alone slots at
    once rather than one collection later. With verify off, the sweep
    would keep such an object without its references having been traced. Verify unmarks it first,
    so a reachable one is also counted in `misses`. Without a cycle it is always 0.
  - Without a cycle, `floating` counts unreachable blocks that were already `HB_Alive` before
    `Mark()` ran (created since the previous collection).
    - In practice these are size-1 slots that stand alone in the heap, for example field slots left
      behind when `SetReflection` restamps an object header to one block.
    - The union keeps them for one more collection, the same lifetime as with verify off.
  - After a cycle, `floating` is the cycle's floating garbage: objects unreachable by the end of the
    cycle that it marked anyway. These include objects allocated during the cycle and the targets of
    weak delegate lists.
  - `NANOCLR_GC_VERIFY_INJECT=N` drops every Nth marked object from the snapshot, which tests the
    detector.
    - A dropped object that is still reachable must come back as a miss.
    - A dropped object that lands on one of those unreachable blocks counts as `dropped_dead`, so
      `misses + dropped_dead == injected` on every collection.
  - Verify mode marks twice per collection, so `NANO_POSIX_GC_TIMING` mark times are not
    comparable while it is on.

## POSIX Networking (macOS and Linux, including WSL)

The socket scheduler, resolver, managed networking bridge and mbedTLS
implementation live in [`src/PAL/Network`](../../src/PAL/Network).
They are also compiled by the
[Windows netcore virtual device](../netcore/README.md), using Winsock and Windows
adapter enumeration. macOS and Linux use their native sockets and resolvers.

macOS and Linux builds register `System.Net` and use native BSD/POSIX sockets directly. No plugin,
embedded TCP/IP stack, desktop .NET relay, root privileges, or network-interface
configuration is required. The application uses the host's network namespace,
routing, DNS configuration, permissions and firewall rules.

Supported in this first implementation:

- IPv4 TCP client/server: connect, bind, listen, accept, send/receive, half-close,
  local/remote endpoints and EOF.
- IPv4 UDP, including connected UDP, one-message-per-call boundaries, source
  endpoints and zero-length datagrams.
- Nonblocking native descriptors with managed waits and timeouts handled by the
  CLR scheduler. A one-shot `poll` worker wakes the CLR's existing socket event;
  idle sockets are not repeatedly polled for writable readiness.
- Native DNS (`getaddrinfo`, including `/etc/hosts`) on a worker thread, returning
  IPv4 results. The CLR remains schedulable while resolution is pending. Each
  managed lookup has a 30-second deadline; up to 16 lookups can be outstanding.
- Numeric IPv4 parsing/formatting and `NetworkInterface.GetIsNetworkAvailable()`.
  Availability means an up/running, non-loopback/non-point-to-point IPv4 interface exists, not that
  an Internet service or DNS server is reachable. On macOS only Ethernet-compatible
  interfaces (including Wi-Fi) qualify.
- Read-only `NetworkInterface.GetAllNetworkInterfaces()`: actual host interfaces,
  sorted by native interface index. Linux includes loopback and currently
  unconfigured links. macOS exposes the main IPv4 loopback interface (`127.0.0.1`,
  normally `lo0`) and up/running Ethernet-compatible interfaces (including Wi-Fi)
  with a nonzero IPv4 address, excluding point-to-point/VPN tunnels, other
  non-Ethernet links and unused adapters. Loopback alone does not make
  `GetIsNetworkAvailable()` return true. Entries are grouped by native
  interface index, not merged by MAC or IP address; distinct adapters remain distinct.
  This filters discovery only: sockets still use the host's routing policy,
  including VPNs, and loopback sockets remain usable.
  Reports one IPv4 address/subnet per link, a default IPv4 gateway, hardware
  address and up to two system-wide IPv4 DNS entries. Linux reads gateways from
  `/proc/net/route` (lowest metric per interface), hardware addresses from
  `AF_PACKET`, and DNS from `/etc/resolv.conf`. macOS reads gateways from the
  Darwin routing table via `sysctl`, hardware addresses from `AF_LINK`, and DNS
  from SystemConfiguration's active global DNS state, not `/etc/resolv.conf`.
  If the macOS DNS store cannot be opened, the failure is logged and adapters
  remain available with DNS addresses reported as `0.0.0.0`.
  The resolver itself still uses the host's native resolver, including split-DNS
  and macOS scoped resolver policy.
- Common integer socket options: reuse address, keepalive, broadcast, don't route,
  inline out-of-band data, send/receive buffer sizes, accept status, socket error,
  TCP no-delay, IPv4 TTL/TOS; `FIONREAD`/`Socket.Available`.
- `Linger`/`DontLinger` socket options translated to Linux `SO_LINGER`.
  Scalar `Linger` values are timeouts in seconds; zero enables an abortive close
  (including when passed as `false` through the managed Boolean overload).
  Set `DontLinger` to `true` to disable lingering. Getting `Linger` returns the
  timeout, while getting `DontLinger` reports whether lingering is disabled.
  The `Linger` byte-array overload also accepts the two 32-bit fields
  (enabled flag, timeout in seconds). Negative timeouts are rejected.
- TLS clients and HTTPS servers using statically linked mbedTLS, TLS 1.2/1.3, configurable CA trust
  or explicit managed CA certificates/bundles; see [POSIX TLS](#posix-tls).

The native socket table is limited to 128 live sockets. CLR handles are independent
of OS descriptor numbers. All descriptors are nonblocking and close-on-exec;
closed handles cannot accidentally identify newly opened sockets. Connections and
resolver requests are discarded on runtime cleanup/soft reboot. Final shutdown
joins both workers before unloading native code.

### Managed loopback example (macOS, Linux or WSL)

Build a nanoFramework console application referencing `System.Net` and using the
following entry point. Use CoreLibrary/System.Net assemblies and the metadata
processor matching this branch's native contracts and PE format; arbitrary
released NuGet assemblies may not match. Deploy the dependency PEs before the
application PE, as in [Running Managed Applications](#running-managed-applications).

All relative PE paths are resolved against the shell's current working directory,
not against the runner's directory. Use absolute paths if launching from elsewhere.
Error `0x9b000000` means an unsupported assembly format: this branch requires
`NFMRK2`, not the `NFMRK1` files produced by older metadata processors. Rebuild all
managed dependencies with matching tools/contracts; renaming files or changing
their format marker does not convert them.

In another terminal on the **same host/network namespace**, start a
one-connection TCP echo server from Bash using `socat` (install on Ubuntu/WSL
with `sudo apt install socat`):

```bash
socat TCP4-LISTEN:9090,bind=127.0.0.1,reuseaddr EXEC:/bin/cat
```

On macOS, install `socat` with `brew install socat` and use the same command.

Run the following client as the **nanoFramework application**:

```csharp
using System;
using System.Diagnostics;
using System.Net;
using System.Net.Sockets;

public class Program
{
    public static void Main()
    {
        IPHostEntry localhost = Dns.GetHostEntry("localhost");
        if (localhost.AddressList.Length == 0)
        {
            throw new Exception("DNS returned no IPv4 addresses.");
        }

        using (Socket socket = new Socket(
            AddressFamily.InterNetwork, SocketType.Stream, ProtocolType.Tcp))
        {
            socket.ReceiveTimeout = 3000;
            socket.SendTimeout = 3000;
            socket.Connect(new IPEndPoint(IPAddress.Parse("127.0.0.1"), 9090));
            byte[] message = new byte[] { 80, 73, 78, 71 };
            socket.Send(message);
            byte[] reply = new byte[message.Length];
            int received = 0;
            while (received < reply.Length)
            {
                int count = socket.Receive(
                    reply, received, reply.Length - received, SocketFlags.None);
                if (count == 0)
                {
                    throw new Exception("Unexpected TCP EOF.");
                }
                received += count;
            }
            for (int i = 0; i < message.Length; ++i)
            {
                if (reply[i] != message[i])
                {
                    throw new Exception("Incorrect echo.");
                }
            }
        }
        Debug.WriteLine("DNS lookup and TCP echo completed.");
    }
}
```

Loopback refers to the namespace of the process running nanoCLR: a Docker
container's loopback is not the WSL host's loopback. Run both peers together, or
deliberately expose/use a reachable host endpoint. Reaching services on Windows
depends on the WSL networking mode and Windows firewall; that is host setup, not
virtual-device driver configuration.

### Deliberately unsupported

IPv6, mutual TLS, multicast-specific options, Wi-Fi provisioning,
SNTP and interface reconfiguration are not implemented here. Native
blocking mode and unmapped flags/options fail explicitly. Network-change events
are not raised. `NetworkHelper`/DHCP/static-IP operations should not be used to
configure the host; the host OS already owns that configuration.

Interface objects are snapshots; a new enumeration refreshes them. Native refresh
after a rejected managed configuration change uses the stored native interface
index, not its position in the enumeration. The count/enumerate pair uses one
snapshot so link changes cannot shift entries mid-enumeration. Loopback and
non-Ethernet links report `Unknown`; Ethernet-compatible links report `Ethernet`,
without claiming wireless provisioning capabilities. DHCP and automatic-DNS
flags are false because a snapshot cannot determine the host's configuration
policy, not because the host must be statically configured. Missing addresses or
default gateways are `0.0.0.0`; no adapter or connectivity is fabricated.

The OS resolver cannot be forcibly cancelled safely: a managed timeout/cancel
discards the request, but an active native lookup may finish later. A slow lookup
delays subsequent queued lookups, and final shutdown waits for that active OS
call. Readiness waiting remains independent of DNS.

For Windows netcore networking support, see the
[Windows networking documentation](../netcore/README.md). Application sockets do
not provide or enable debugger/wire-protocol transport.

## POSIX TLS

macOS and Linux TLS use the repository-pinned mbedTLS, the host clock and operating-system
entropy. It does not automatically load the host's certificate store.
In particular, macOS Keychain trust is not imported.
For outgoing client connections, chain verification, validity dates and the
requested DNS hostname/IP identity are checked by default. SNI is sent for DNS names.
`SslProtocols.None` negotiates TLS 1.2/1.3; TLS 1.0/1.1 are unsupported.

Explicitly setting `SslStream.SslVerification = SslVerification.NoVerification`
before `AuthenticateAsClient` disables certificate chain, date and hostname/IP
verification for that context only. This mode does not load or require CA roots,
and ignores managed CA certificates/bundles and configured CA files. DNS SNI is
still sent. **Use this only for testing or deliberately unauthenticated TLS: the
connection remains encrypted but is vulnerable to impersonation and man-in-the-middle
attacks.** The default and all other supported verification modes still require
valid trust; errors never automatically switch to `NoVerification`.
Transport failures during TLS reads/writes retain their socket error codes and
raise `SocketException`; TLS protocol/cryptographic failures raise `CryptographicException`.

Trust selection, in order:

1. A CA passed through `HttpWebRequest.HttpsAuthentCert` or
   `SslStream.AuthenticateAsClient(..., ca, ...)` is the **exclusive** trust store
   for that TLS context. It is not appended to any default roots.
2. If no per-connection CA is supplied and
   `CertificateManager.AddCaCertificateBundle(byte[])` was called, the supplied
   process-local bundle is the exclusive default for subsequently created contexts.
   It is cleared on runtime cleanup/soft reboot and is not persisted on disk.
3. Otherwise the runner's **`--ca-bundle <path>` option** selects a CA file,
   overriding `NANOCLR_CA_BUNDLE` for that process.
4. Without the option, **`NANOCLR_CA_BUNDLE`** selects a CA file. This also works
   when another application loads the native runtime library.

The CA path is optional. With neither option nor environment variable, only
certificates supplied by managed code (per connection or through the global
bundle) can be used. Without any trust source, verified outgoing TLS fails explicitly;
certificate verification is only disabled by explicit `NoVerification`. No CA roots are embedded or shipped,
and the host store is not used automatically. HTTPS servers do not require a CA
path; they use their supplied server certificate and private key.

For example, configure a PEM CA bundle using the environment:

```bash
export NANOCLR_CA_BUNDLE=/absolute/path/to/company-ca.pem
```

Or add the option before the PE arguments in your launch command:

```bash
./build/posix/bin/nanoFramework.nanoCLR.test --ca-bundle /absolute/path/to/company-ca.pem \
    mscorlib.pe \
    nanoFramework.Runtime.Events.pe \
    <additional dependency PEs...> \
    MyApplication.pe
```

The option is supported on both macOS and Linux. Relative custom
paths are resolved against the process's working directory. CA files may contain
a PEM bundle or one DER certificate, up to 1 MiB. A missing, empty, malformed or
unreadable selected file fails TLS explicitly: it never falls back to another
file or the host store. Per-request certificates and the managed global bundle
take precedence over file configuration. File changes affect newly created TLS
contexts; existing contexts retain their original trust.

To opt into a host-provided PEM bundle, explicitly set `NANOCLR_CA_BUNDLE` to its
path. `SSL_CERT_FILE` and `SSL_CERT_DIR` are not used. In a container, any configured
path must exist inside the container.

Explicit certificates accept a PEM certificate/bundle or one DER certificate.
Empty/malformed certificates and verification failures are errors: they never
fall back to default trust. Existing TLS contexts retain their original trust when
the process-wide default changes.

For example:

```csharp
using (HttpWebRequest request = (HttpWebRequest)WebRequest.Create("https://www.bing.com/"))
{
    request.SslProtocols = SslProtocols.Tls12;
    request.SslVerification = SslVerification.VerifyPeer;
    request.Timeout = 30000;
    request.ReadWriteTimeout = 30000;

    // Optional: the PEM contents of the CA certificate(s), not a file path.
    // Omit only if a CA file or managed global bundle is configured.
    request.HttpsAuthentCert = new X509Certificate(caPem);

    using (HttpWebResponse response = (HttpWebResponse)request.GetResponse())
    {
        // Consume the response stream as usual.
    }
}
```

The certificate parameter above is a trust anchor, not a client identity or
certificate pin. Outgoing client certificates, mutual TLS, stored device
certificates, and revocation fetching/OCSP are not implemented. X509Certificate
issuer/subject/validity metadata and X509Certificate2 private keys are parsed
using mbedTLS.

TLS uses nonblocking send/receive callbacks and the existing one-shot socket event worker.
Handshakes and reads/writes yield to the CLR on either WANT_READ or WANT_WRITE,
including when a read needs to write TLS protocol traffic. Decrypted buffered
bytes are available without waiting for another socket event. Handshakes have
a 30-second deadline; I/O uses the existing managed stream timeout. Contexts
are limited to 16, are invalidated on cleanup, and are freed on managed disposal.
TLS errors are logged with mbedTLS diagnostics and surfaced as managed
cryptographic exceptions (HTTP may wrap these in `WebException`).

### HTTPS servers

`nanoFramework.WebServer` and `SslStream.AuthenticateAsServer` can serve HTTPS.
Supply an `X509Certificate2` containing the server certificate and its matching
private key. Certificate chains use PEM with the leaf first, followed by
intermediates; a single DER certificate is also accepted. Private keys accept
PEM or DER, including encrypted formats supported by mbedTLS, with the password
passed to `X509Certificate2`. Missing keys, malformed keys, incorrect passwords
and certificate/key mismatches fail explicitly.

For a local development certificate, run this Bash command on the host
(OpenSSL is only a certificate-generation tool here, not a runtime dependency):

```bash
openssl req -x509 -newkey rsa:2048 -sha256 -days 7 -nodes \
    -subj "/CN=localhost" \
    -addext "subjectAltName=DNS:localhost,IP:127.0.0.1" \
    -addext "basicConstraints=critical,CA:FALSE" \
    -addext "extendedKeyUsage=serverAuth" \
    -keyout server-key.pem -out server-cert.pem
```

Provide the generated PEM **contents**, not their file paths, to your application,
for example through private development resources. Managed filesystem access
is not implemented on this target. Do not commit private keys or publish them
with application packages.

```csharp
using System;
using System.Net.Security;
using System.Security.Cryptography.X509Certificates;
using System.Threading;
using nanoFramework.WebServer;

public static class HttpsHost
{
    public static void Run(string certificatePem, string privateKeyPem, string password = null)
    {
        var identity = new X509Certificate2(certificatePem, privateKeyPem, password);
        using (var server = new WebServer(8443, HttpProtocol.Https))
        {
            server.HttpsCert = identity;
            server.SslProtocols = SslProtocols.None;
            server.CommandReceived += (sender, e) =>
            {
                e.Context.Response.ContentType = "text/plain";
                WebServer.OutputAsStream(e.Context.Response, "Hello from nanoFramework HTTPS");
            };
            if (!server.Start())
            {
                throw new Exception("HTTPS server failed to start.");
            }
            Thread.Sleep(Timeout.Infinite);
        }
    }
}
```

Use compatible managed `nanoFramework.WebServer` and
`nanoFramework.System.Net.Http.Server` assemblies plus their dependencies.
The HTTP server package provides `System.Net.Http`; do not deploy the client-only
assembly in its place. All assemblies must match this branch's runtime/PE format.
WebServer sources targeting the older `SpanByte` API need porting to
`Span<byte>` when rebuilt against the 2.0 CoreLibrary.

From the same WSL distribution/network namespace, verify the server without
disabling certificate checks:

```bash
curl --cacert server-cert.pem https://localhost:8443/
```

The hostname/IP must match the server certificate's SAN, and clients must trust
its issuing CA (or the self-signed certificate for this local example). Port
8443 does not require root. Limit network exposure using the host firewall;
reachability from Windows or another machine depends on WSL networking.

The outgoing CA bundle setting does **not** supply a server identity or configure
which clients can connect. Servers do not request client certificates; mutual
TLS requests are rejected rather than silently downgraded. For direct SslStream
use, call `AuthenticateAsServer(identity, false, SslProtocols.None)`.
Client-side certificate verification remains the default; only an explicit
`SslVerification.NoVerification` setting disables it.

## Running Managed Applications

The runner exits with a nonzero status if host networking initialization fails,
instead of reporting success without starting the CLR. It uses the POSIX
`nanoCLR_RunWithStatus` export; the existing `void nanoCLR_Run` export remains
unchanged for managed hosts. This status does not report managed application
failures.

Pass the application's PE file and all matching dependency PEs directly to the
standalone runner, with dependencies before the application:

```bash
./build/posix/bin/nanoFramework.nanoCLR.test \
    mscorlib.pe \
    nanoFramework.Runtime.Events.pe \
    <additional dependency PEs...> \
    MyApplication.pe
```

Include only the dependencies your application references. For networking/HTTPS,
these may include `System.Net`, `System.Net.Http`, `System.IO.Streams`,
`nanoFramework.System.Text`, `nanoFramework.System.Collections` and
`System.Threading`, plus their dependencies. All PEs must match this runtime's
native contracts and assembly format.

The managed heap is 10 MiB. Set `NANOCLR_HEAP_SIZE` to a size in decimal bytes (1 MiB to 1 GiB) to
change it; an invalid value prints a warning and keeps the default. The heap never grows, and it is
filled when allocated, so all of it is committed at startup.

## POSIX Shim Headers

`targets/posix/Include/` contains shim headers searched **before** the shared CLR/HAL/PAL paths. They shadow headers that would otherwise pull in Windows-only content (e.g. `nanoPAL.h`, `nanoHAL_Boot.h`) and provide no-op replacements for optional subsystems not present on a POSIX host.

The macro `PLATFORM_POSIX_HOST` is defined by the build system and gates POSIX-specific branches in the shared headers.

## Native Assembly Table

The interop table in `posix_stubs.cpp` registers the same portable set as the win32 virtual-device target. Hardware assemblies (GPIO, I2C, SPI, IO.Ports) are included with `NotImplementedStub()` bodies — they satisfy the PE checksum check at load time so applications referencing these assemblies can load, but calling their native methods throws `NotImplementedException`. Both macOS and Linux additionally register the socket-backed `System.Net` assembly described above.

| Assembly | Notes |
|---|---|
| `mscorlib` | Full native implementation |
| `nanoFramework.Runtime.Native` | Full |
| `nanoFramework.System.Collections` | Full |
| `nanoFramework.System.Text` | Full |
| `System.Math` | Full |
| `System.Runtime.Serialization` | Full |
| `nanoFramework.ResourceManager` | Full |
| `nanoFramework.System.IO.Hashing` | Full (CRC32) |
| `nanoFramework.Runtime.Events` | Full |
| `nanoFramework.Runtime.Events_EventSink_DriverProcs` | Full |
| `System.Device.Gpio` | Stubs — `NotImplementedException` |
| `System.Device.I2c` | Stubs — `NotImplementedException` |
| `System.Device.Spi` | Stubs — `NotImplementedException` |
| `System.IO.Ports` | Stubs — `NotImplementedException` |
| `System.Net` | macOS and Linux: native IPv4 sockets/DNS, read-only interfaces, TLS clients and HTTPS servers; other APIs remain unsupported |

Not included (require a network stack or platform filesystem not present on the host):
`nanoFramework.Networking.Sntp`, `System.IO.FileSystem`.

## Known Limitations

- macOS and Linux networking is IPv4-only; TLS clients and HTTPS servers are supported with the limitations above.
- No managed filesystem (`System.IO.FileSystem`) — PAL filesystem calls are no-ops.
- `System.Device.Gpio/I2c/Spi` and `System.IO.Ports` native methods throw `NotImplementedException`.
- Wire Protocol transport is not connected (debugger attach not supported).
