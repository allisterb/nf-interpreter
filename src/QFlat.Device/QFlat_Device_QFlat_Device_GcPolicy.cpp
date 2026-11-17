//
// Copyright (c) .NET Foundation and Contributors
// See LICENSE file in the project root for full license information.
//

// QFlat.Device.GcPolicy natives: no-GC regions (nanoCLR_GcNoGcRegion.h), statistics (nanoCLR_GcStats.h) and the
// run-time profile (nanoCLR_GcIncremental.h).

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

    stack.SetResult_I8((CLR_INT64)GcStats_WorkUnits());

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

HRESULT Library_QFlat_Device_QFlat_Device_GcPolicy::ResetPauseStats___STATIC__VOID(CLR_RT_StackFrame &stack)
{
    NANOCLR_HEADER();
    (void)stack;

    GcStats_ResetPauses();

    NANOCLR_NOCLEANUP_NOLABEL();
}

HRESULT Library_QFlat_Device_QFlat_Device_GcPolicy::NativeGetProfile___STATIC__I4(CLR_RT_StackFrame &stack)
{
    NANOCLR_HEADER();

    stack.SetResult_I4(NANOCLR_GC_PROFILE());

    NANOCLR_NOCLEANUP_NOLABEL();
}

HRESULT Library_QFlat_Device_QFlat_Device_GcPolicy::NativeSetProfile___STATIC__VOID__I4(CLR_RT_StackFrame &stack)
{
    NANOCLR_HEADER();

    NANOCLR_SET_AND_LEAVE(NANOCLR_GC_SET_PROFILE(stack.Arg0().NumericByRef().s4));

    NANOCLR_NOCLEANUP();
}

HRESULT Library_QFlat_Device_QFlat_Device_GcPolicy::NativeGetStats___STATIC__VOID__SZARRAY_I8(CLR_RT_StackFrame &stack)
{
    NANOCLR_HEADER();

    CLR_RT_HeapBlock_Array *values = stack.Arg0().DereferenceArray();
    FAULT_ON_NULL(values);

    if (values->m_typeOfElement != DATATYPE_I8)
    {
        NANOCLR_SET_AND_LEAVE(CLR_E_WRONG_TYPE);
    }

    GcStats_Get((CLR_INT64 *)values->GetFirstElement(), values->m_numOfElements);

    NANOCLR_NOCLEANUP();
}
