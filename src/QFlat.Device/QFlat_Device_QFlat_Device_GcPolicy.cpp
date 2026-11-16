//
// Copyright (c) .NET Foundation and Contributors
// See LICENSE file in the project root for full license information.
//

// QFlat.Device.GcPolicy natives: no-GC regions (nanoCLR_GcNoGcRegion.h).

#include "QFlat_Device.h"

HRESULT Library_QFlat_Device_QFlat_Device_GcPolicy::get_InNoGcRegion___STATIC__BOOLEAN(CLR_RT_StackFrame &stack)
{
    NANOCLR_HEADER();

    stack.SetResult_Boolean(GcNoGcRegion_InRegion());

    NANOCLR_NOCLEANUP_NOLABEL();
}

HRESULT Library_QFlat_Device_QFlat_Device_GcPolicy::get_GcWorkCount___STATIC__I8(CLR_RT_StackFrame &stack)
{
    NANOCLR_HEADER();

    stack.SetResult_I8((CLR_INT64)GcNoGcRegion_WorkCount());

    NANOCLR_NOCLEANUP_NOLABEL();
}

HRESULT Library_QFlat_Device_QFlat_Device_GcPolicy::EndNoGcRegion___STATIC__BOOLEAN(CLR_RT_StackFrame &stack)
{
    NANOCLR_HEADER();

    stack.SetResult_Boolean(GcNoGcRegion_End());

    NANOCLR_NOCLEANUP_NOLABEL();
}

HRESULT Library_QFlat_Device_QFlat_Device_GcPolicy::NativeStartNoGcRegion___STATIC__BOOLEAN__I4(
    CLR_RT_StackFrame &stack)
{
    NANOCLR_HEADER();

    bool started;

    NANOCLR_CHECK_HRESULT(GcNoGcRegion_Start(stack.Arg0().NumericByRef().s4, started));
    stack.SetResult_Boolean(started);

    NANOCLR_NOCLEANUP();
}
