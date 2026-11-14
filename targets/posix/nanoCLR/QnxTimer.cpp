//
// Copyright (c) .NET Foundation and Contributors
// See LICENSE file in the project root for full license information.
//

// QNX kernel timers and pulses in place of the posix host's thread per quantum, thread per completion timer and
// 10 ms event polling (posix README, NANO_POSIX_QNX_TIMER).
//
// - Two CLOCK_MONOTONIC timers, quantum and completion, send pulses to a private channel. One receiver thread
//   blocks in MsgReceivePulse. The pulses carry the CLR thread's priority + 1, so the receiver preempts the
//   interpreter on the same CPU as soon as a timer expires.
// - A timer is re-armed in place, so a pulse from an earlier arming can still be queued. Each timer therefore
//   keeps its current absolute deadline, under its own lock; a pulse acts only if that deadline is set and has
//   passed, and takes it. A pulse that arrives before the deadline re-arms the timer at it, so neither a stale
//   pulse nor an early one can lose the current expiry.
// - The idle wait (Events_WaitForEvents) blocks in MsgReceivePulse on a second channel with an absolute
//   TimerTimeout, and Events_Set sends a pulse there only while a waiter is registered. That channel has
//   _NTO_CHF_FIXED_PRIORITY, so the pulse never changes the interpreter's priority.
// - Ordinary QNX timers expire on the clock tick (1 ms by default), so a deadline is met up to a tick late; a
//   sleeper that wakes on a tick and sleeps a whole number of ms is always nearly a tick late. A tolerance below
//   the tick makes both timers and the idle timeout high-resolution (NANOCLR_QNX_TIMER_TOLERANCE_NS, default
//   1000; 0 keeps tick timers). That needs the PROCMGR_AID_HIGH_RESOLUTION_TIMER ability, which root has.

#include "QnxTimer.h"

#if defined(__QNX__)

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <pthread.h>
#include <sched.h>
#include <sys/neutrino.h>
#include <sys/siginfo.h>
#include <time.h>

#include <nanoHAL.h>
#include <nanoPAL_Events.h>

#if defined(NANOCLR_GC_TIMING)
#include <nanoCLR_GcTiming.h>
#endif

namespace
{
    const int c_codeQuantum = _PULSE_CODE_MINAVAIL;
    const int c_codeCompare = _PULSE_CODE_MINAVAIL + 1;
    const int c_codeWake = _PULSE_CODE_MINAVAIL + 2;

    struct Timer
    {
        timer_t id;
        pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
        uint64_t deadlineNs = 0; // absolute CLOCK_MONOTONIC; 0 = disarmed
        bool *flag = nullptr;    // quantum only: set at the deadline
    };

    // Written once by Setup, before the receiver thread exists.
    bool s_enabled = false;
    uint64_t s_toleranceNs = 0; // 0 = tick-resolution timers
    int s_timerChid = -1;
    int s_waitCoid = -1;
    int s_waitChid = -1;
    int s_clrPriority = 0;
    Timer s_quantum;
    Timer s_compare;
    std::atomic<bool> s_waiting{false};

    // Under timer.lock. Arms at deadlineNs, or disarms for 0; a deadline already passed fires at once.
    bool Arm(Timer &timer, uint64_t deadlineNs)
    {
        timer.deadlineNs = deadlineNs;
        itimerspec spec{};
        spec.it_value.tv_sec = (time_t)(deadlineNs / 1000000000ULL);
        spec.it_value.tv_nsec = (long)(deadlineNs % 1000000000ULL);
        if (timer_settime(timer.id, TIMER_ABSTIME, &spec, nullptr) == 0)
        {
            return true;
        }

        timer.deadlineNs = 0;
        return false;
    }

    // On a pulse: true if the deadline has passed, after taking it (and, for the quantum, setting the flag).
    bool Expire(Timer &timer)
    {
        pthread_mutex_lock(&timer.lock);
        const uint64_t due = timer.deadlineNs;
        // If re-arming fails, expire now rather than lose the expiry.
        const bool expired = due != 0 && (QnxTimer_NowNs() >= due || !Arm(timer, due));
        if (expired)
        {
            timer.deadlineNs = 0;
            if (timer.flag != nullptr)
            {
#if defined(NANOCLR_GC_TIMING)
                GcTiming_QuantumFired();
#endif
                *timer.flag = true;
                timer.flag = nullptr;
            }
        }

        pthread_mutex_unlock(&timer.lock);
        return expired;
    }

    void *Receive(void *)
    {
        for (;;)
        {
            _pulse pulse;
            if (MsgReceivePulse(s_timerChid, &pulse, sizeof(pulse), nullptr) == -1)
            {
                if (errno == EINTR)
                {
                    continue;
                }

                std::fprintf(stderr, "[QNX] timer receiver stopped: %s\n", std::strerror(errno));
                return nullptr;
            }

            if (pulse.code == c_codeQuantum && Expire(s_quantum))
            {
                Events_Set(SYSTEM_EVENT_FLAG_SYSTEM_TIMER);
            }
            else if (pulse.code == c_codeCompare && Expire(s_compare))
            {
                // Outside the lock: it re-arms through Time_SetCompare.
                HAL_COMPLETION::DequeueAndExec();
                Events_Set(SYSTEM_EVENT_FLAG_SYSTEM_TIMER);
            }
        }
    }

    bool Fail(const char *what)
    {
        std::fprintf(stderr, "[QNX] kernel timer off, %s failed: %s\n", what, std::strerror(errno));
        return false;
    }

    // An unprivileged process may not send pulses above its priority limit, so fall back to the CLR's own.
    bool CreateTimer(Timer &timer, int coid, int &priority, int code)
    {
        sigevent event;
        SIGEV_PULSE_INIT(&event, coid, priority, code, nullptr);
        if (timer_create(CLOCK_MONOTONIC, &event, &timer.id) == 0)
        {
            return true;
        }

        if (errno != EPERM || priority == s_clrPriority)
        {
            return false;
        }

        priority = s_clrPriority;
        SIGEV_PULSE_INIT(&event, coid, priority, code, nullptr);
        return timer_create(CLOCK_MONOTONIC, &event, &timer.id) == 0;
    }

    bool Setup()
    {
        const char *value = std::getenv("NANOCLR_QNX_TIMER");
        if (value != nullptr && std::strcmp(value, "0") == 0)
        {
            return false;
        }

        int policy;
        sched_param param;
        if (pthread_getschedparam(pthread_self(), &policy, &param) != 0)
        {
            return Fail("pthread_getschedparam");
        }

        s_clrPriority = param.sched_priority;
        int timerPriority = s_clrPriority < sched_get_priority_max(policy) ? s_clrPriority + 1 : s_clrPriority;

        s_timerChid = ChannelCreate(_NTO_CHF_PRIVATE);
        if (s_timerChid == -1)
        {
            return Fail("ChannelCreate");
        }

        const int timerCoid = ConnectAttach(0, 0, s_timerChid, _NTO_SIDE_CHANNEL, 0);
        if (timerCoid == -1)
        {
            return Fail("ConnectAttach");
        }

        s_waitChid = ChannelCreate(_NTO_CHF_PRIVATE | _NTO_CHF_FIXED_PRIORITY);
        if (s_waitChid == -1)
        {
            return Fail("ChannelCreate");
        }

        s_waitCoid = ConnectAttach(0, 0, s_waitChid, _NTO_SIDE_CHANNEL, 0);
        if (s_waitCoid == -1)
        {
            return Fail("ConnectAttach");
        }

        if (!CreateTimer(s_quantum, timerCoid, timerPriority, c_codeQuantum) ||
            !CreateTimer(s_compare, timerCoid, timerPriority, c_codeCompare))
        {
            return Fail("timer_create");
        }

        const char *tolerance = std::getenv("NANOCLR_QNX_TIMER_TOLERANCE_NS");
        s_toleranceNs = tolerance != nullptr ? std::strtoull(tolerance, nullptr, 10) : 1000;
        if (s_toleranceNs != 0)
        {
            itimerspec spec{};
            spec.it_value.tv_nsec = (long)(s_toleranceNs % 1000000000ULL);
            spec.it_value.tv_sec = (time_t)(s_toleranceNs / 1000000000ULL);
            if (timer_settime(s_quantum.id, TIMER_TOLERANCE, &spec, nullptr) == -1 ||
                timer_settime(s_compare.id, TIMER_TOLERANCE, &spec, nullptr) == -1)
            {
                std::fprintf(stderr, "[QNX] tick timers, TIMER_TOLERANCE failed: %s\n", std::strerror(errno));
                s_toleranceNs = 0;
            }
        }

        s_enabled = true;
        pthread_t thread;
        if (pthread_create(&thread, nullptr, Receive, nullptr) != 0)
        {
            s_enabled = false;
            return Fail("pthread_create");
        }

        pthread_setname_np(thread, "nanoCLR timer");
        pthread_detach(thread);

#if defined(NANOCLR_GC_TIMING)
        std::printf(
            "[QNX] kernel timer on, clr_priority=%d timer_priority=%d tolerance_ns=%llu\r\n",
            s_clrPriority,
            timerPriority,
            (unsigned long long)s_toleranceNs);
#endif
        return true;
    }
} // namespace

uint64_t QnxTimer_NowNs()
{
    timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000000000ULL + (uint64_t)now.tv_nsec;
}

bool QnxTimer_Initialize()
{
    // Events_Initialize runs on the CLR thread, so Setup reads that thread's priority.
    static const bool enabled = Setup();
    return enabled;
}

void QnxTimer_Uninitialize()
{
    QnxTimer_SetQuantum(nullptr, 0);
    QnxTimer_SetCompare(HAL_COMPLETION_IDLE_VALUE);
}

bool QnxTimer_SetQuantum(bool *flag, uint64_t deadlineNs)
{
    if (!s_enabled)
    {
        return false;
    }

    pthread_mutex_lock(&s_quantum.lock);
    s_quantum.flag = flag;
    const bool armed = Arm(s_quantum, flag == nullptr ? 0 : deadlineNs == 0 ? 1 : deadlineNs);
    if (!armed)
    {
        s_quantum.flag = nullptr;
    }

    pthread_mutex_unlock(&s_quantum.lock);
    return armed;
}

bool QnxTimer_SetCompare(uint64_t compareTicks)
{
    if (!s_enabled)
    {
        return false;
    }

    // 0 means disarmed, so a compare value of 0 (long past) becomes 1 ns.
    const uint64_t deadline = compareTicks >= HAL_COMPLETION_IDLE_VALUE ? 0
                              : compareTicks == 0                      ? 1
                                                                       : compareTicks * 100ULL;
    pthread_mutex_lock(&s_compare.lock);
    const bool armed = Arm(s_compare, deadline);
    pthread_mutex_unlock(&s_compare.lock);
    return armed;
}

void QnxTimer_BeginWait()
{
    s_waiting.store(true, std::memory_order_seq_cst);
}

void QnxTimer_EndWait()
{
    s_waiting.store(false, std::memory_order_seq_cst);
}

void QnxTimer_WaitUntil(uint64_t deadlineNs)
{
    _pulse pulse;
    if (s_toleranceNs != 0)
    {
        TimerTimeout(CLOCK_MONOTONIC, TIMER_TOLERANCE, nullptr, &s_toleranceNs, nullptr);
    }

    TimerTimeout(CLOCK_MONOTONIC, _NTO_TIMEOUT_RECEIVE | TIMER_ABSTIME, nullptr, &deadlineNs, nullptr);
    // Returns on a pulse, at the deadline (ETIMEDOUT) or on a signal; the caller re-checks in every case.
    MsgReceivePulse(s_waitChid, &pulse, sizeof(pulse), nullptr);
}

void QnxTimer_Wake()
{
    if (s_enabled && s_waiting.exchange(false, std::memory_order_seq_cst))
    {
        MsgSendPulse(s_waitCoid, s_clrPriority, c_codeWake, 0);
    }
}

#endif // __QNX__
