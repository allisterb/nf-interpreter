//
// Copyright (c) .NET Foundation and Contributors
// See LICENSE file in the project root for full license information.
//

#pragma once

#include <cstdint>

// QNX kernel timers and pulses for the posix host; built on QNX when NANO_POSIX_QNX_TIMER is ON (define
// NANOCLR_QNX_TIMER; see the posix README). While the module is off (NANOCLR_QNX_TIMER=0 in the environment, or
// setup failed) the Set functions return false and the callers fall back to the portable code.

// Creates the channels, timers and receiver thread once per process; later calls only report the state.
bool QnxTimer_Initialize();

// Disarms both timers (soft reboot); the channels and the thread stay for the next Initialize.
void QnxTimer_Uninitialize();

// Arms the quantum timer to set *flag at deadlineNs (CLOCK_MONOTONIC); flag == nullptr disarms it.
bool QnxTimer_SetQuantum(bool *flag, uint64_t deadlineNs);

// Arms the completion timer at compareTicks (HAL time: CLOCK_MONOTONIC in 100 ns units); HAL_COMPLETION_IDLE_VALUE
// or more disarms it. Both Set functions return false if the timer could not be armed.
bool QnxTimer_SetCompare(uint64_t compareTicks);

// Waiter protocol for Events_WaitForEvents, for one waiting thread at a time (on posix, only the CLR thread
// waits). BeginWait before checking the event flags, then WaitUntil, then EndWait. WaitUntil returns on a Wake
// pulse or at deadlineNs (CLOCK_MONOTONIC); a stale pulse only causes a spurious return.
void QnxTimer_BeginWait();
void QnxTimer_WaitUntil(uint64_t deadlineNs);
void QnxTimer_EndWait();

// Wakes a thread inside WaitUntil, if any. Called by Events_Set.
void QnxTimer_Wake();

uint64_t QnxTimer_NowNs();
