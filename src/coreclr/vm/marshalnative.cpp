// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
//
// File: MarshalNative.cpp
//

//
// FCall's for the PInvoke classlibs
//


#include "common.h"
#include "clsload.hpp"
#include "method.hpp"
#include "class.h"
#include "object.h"
#include "field.h"
#include "util.hpp"
#include "excep.h"
#include "siginfo.hpp"
#include "threads.h"
#include "stublink.h"
#include "dllimport.h"
#include "jitinterface.h"
#include "eeconfig.h"
#include "log.h"
#include "fieldmarshaler.h"
#include "cgensys.h"
#include "gcheaputilities.h"
#include "dbginterface.h"
#include "marshalnative.h"
#include "fcall.h"
#include "dllimportcallback.h"
#include "comdelegate.h"
#include "typestring.h"
#include "appdomain.inl"

#ifdef FEATURE_COMINTEROP
#include "comcallablewrapper.h"
#include "commtmemberinfomap.h"
#include "runtimecallablewrapper.h"
#include "olevariant.h"
#include "interoputil.h"
#endif // FEATURE_COMINTEROP

// Prelink
// Does advance loading of an N/Direct library
extern "C" VOID QCALLTYPE MarshalNative_Prelink(MethodDesc * pMD)
{
    QCALL_CONTRACT;

    // Arguments are check on managed side
    PRECONDITION(pMD != NULL);

    // If the code is already ready, we are done. Else, we need to execute the prestub
    // This is a perf thing since it's always safe to execute the prestub twice.
    if (!pMD->IsPointingToPrestub())
        return;

    // Silently ignore if not N/Direct and not runtime generated.
    if (!(pMD->IsNDirect()) && !(pMD->IsRuntimeSupplied()))
        return;

    BEGIN_QCALL;

    pMD->CheckRestore();
    pMD->DoPrestub(NULL);

    END_QCALL;
}

// IsBuiltInComSupported
// Built-in COM support is only checked from the native side to ensure the runtime
// is in a consistent state
extern "C" BOOL QCALLTYPE MarshalNative_IsBuiltInComSupported()
{
    QCALL_CONTRACT;

    BOOL ret = TRUE;

    BEGIN_QCALL;

#ifdef FEATURE_COMINTEROP
    ret = g_pConfig->IsBuiltInCOMSupported();
#else // FEATURE_COMINTEROP
    ret = FALSE;
#endif // FEATURE_COMINTEROP

    END_QCALL;

    return ret;
}

FCIMPL3(VOID, MarshalNative::StructureToPtr, Object* pObjUNSAFE, LPVOID ptr, CLR_BOOL fDeleteOld)
{
    CONTRACTL
    {
        FCALL_CHECK;
        PRECONDITION(CheckPointer(ptr, NULL_OK));
    }
    CONTRACTL_END;

    OBJECTREF pObj = (OBJECTREF) pObjUNSAFE;

    HELPER_METHOD_FRAME_BEGIN_1(pObj);

    if (ptr == NULL)
        COMPlusThrowArgumentNull(W("ptr"));
    if (pObj == NULL)
        COMPlusThrowArgumentNull(W("structure"));

    // Code path will accept both regular layout objects and boxed value classes
    // with layout.

    MethodTable *pMT = pObj->GetMethodTable();

    if (pMT->HasInstantiation())
        COMPlusThrowArgumentException(W("structure"), W("Argument_NeedNonGenericObject"));

    if (pMT->IsBlittable())
    {
        memcpyNoGCRefs(ptr, pObj->GetData(), pMT->GetNativeSize());
    }
    else if (pMT->HasLayout())
    {
        MethodDesc* structMarshalStub;

        {
            GCX_PREEMP();
            structMarshalStub = NDirect::CreateStructMarshalILStub(pMT);
        }

        if (fDeleteOld)
        {
            MarshalStructViaILStub(structMarshalStub, pObj->GetData(), ptr, StructMarshalStubs::MarshalOperation::Cleanup);
        }

        MarshalStructViaILStub(structMarshalStub, pObj->GetData(), ptr, StructMarshalStubs::MarshalOperation::Marshal);
    }
    else
    {
        COMPlusThrowArgumentException(W("structure"), W("Argument_MustHaveLayoutOrBeBlittable"));
    }

    HELPER_METHOD_FRAME_END();
}
FCIMPLEND

FCIMPL3(VOID, MarshalNative::PtrToStructureHelper, LPVOID ptr, Object* pObjIn, CLR_BOOL allowValueClasses)
{
    CONTRACTL
    {
        FCALL_CHECK;
        PRECONDITION(CheckPointer(ptr, NULL_OK));
    }
    CONTRACTL_END;

    OBJECTREF  pObj = ObjectToOBJECTREF(pObjIn);

    HELPER_METHOD_FRAME_BEGIN_1(pObj);

    if (ptr == NULL)
        COMPlusThrowArgumentNull(W("ptr"));
    if (pObj == NULL)
        COMPlusThrowArgumentNull(W("structure"));

    // Code path will accept regular layout objects.
    MethodTable *pMT = pObj->GetMethodTable();

    // Validate that the object passed in is not a value class.
    if (!allowValueClasses && pMT->IsValueType())
    {
        COMPlusThrowArgumentException(W("structure"), W("Argument_StructMustNotBeValueClass"));
    }
    else if (pMT->IsBlittable())
    {
        memcpyNoGCRefs(pObj->GetData(), ptr, pMT->GetNativeSize());
    }
    else if (pMT->HasLayout())
    {
        MethodDesc* structMarshalStub;

        {
            GCX_PREEMP();
            structMarshalStub = NDirect::CreateStructMarshalILStub(pMT);
        }

        MarshalStructViaILStub(structMarshalStub, pObj->GetData(), ptr, StructMarshalStubs::MarshalOperation::Unmarshal);
    }
    else
    {
        COMPlusThrowArgumentException(W("structure"), W("Argument_MustHaveLayoutOrBeBlittable"));
    }

    HELPER_METHOD_FRAME_END();
}
FCIMPLEND


FCIMPL2(VOID, MarshalNative::DestroyStructure, LPVOID ptr, ReflectClassBaseObject* refClassUNSAFE)
{
    CONTRACTL
    {
        FCALL_CHECK;
        PRECONDITION(CheckPointer(ptr, NULL_OK));
    }
    CONTRACTL_END;

    REFLECTCLASSBASEREF refClass = (REFLECTCLASSBASEREF) refClassUNSAFE;
    HELPER_METHOD_FRAME_BEGIN_1(refClass);

    if (ptr == NULL)
        COMPlusThrowArgumentNull(W("ptr"));
    if (refClass == NULL)
        COMPlusThrowArgumentNull(W("structureType"));
    if (refClass->GetMethodTable() != g_pRuntimeTypeClass)
        COMPlusThrowArgumentException(W("structureType"), W("Argument_MustBeRuntimeType"));

    TypeHandle th = refClass->GetType();

    if (th.HasInstantiation())
        COMPlusThrowArgumentException(W("structureType"), W("Argument_NeedNonGenericType"));

    if (th.IsBlittable())
    {
        // ok to call with blittable structure, but no work to do in this case.
    }
    else if (th.HasLayout())
    {
        MethodDesc* structMarshalStub;

        {
            GCX_PREEMP();
            structMarshalStub = NDirect::CreateStructMarshalILStub(th.GetMethodTable());
        }

        MarshalStructViaILStub(structMarshalStub, nullptr, ptr, StructMarshalStubs::MarshalOperation::Cleanup);
    }
    else
    {
        COMPlusThrowArgumentException(W("structureType"), W("Argument_MustHaveLayoutOrBeBlittable"));
    }

    HELPER_METHOD_FRAME_END();
}
FCIMPLEND

/************************************************************************
 * PInvoke.SizeOf(Class)
 */
FCIMPL2(UINT32, MarshalNative::SizeOfClass, ReflectClassBaseObject* refClassUNSAFE, CLR_BOOL throwIfNotMarshalable)
{
    CONTRACTL
    {
        FCALL_CHECK;
        PRECONDITION(CheckPointer(refClassUNSAFE));
    }
    CONTRACTL_END;

    UINT32 rv = 0;
    REFLECTCLASSBASEREF refClass = (REFLECTCLASSBASEREF)refClassUNSAFE;

    HELPER_METHOD_FRAME_BEGIN_RET_1(refClass);

    // refClass is validated to be non-NULL RuntimeType by callers
    TypeHandle th = refClass->GetType();

    if (throwIfNotMarshalable && (!th.IsBlittable() || th.IsArray()))
    {
        GCX_PREEMP();
        // Determine if the type is marshalable
        if (!IsStructMarshalable(th))
        {
            // It isn't marshalable so throw an ArgumentException.
            StackSString strTypeName;
            TypeString::AppendType(strTypeName, th);
            COMPlusThrow(kArgumentException, IDS_CANNOT_MARSHAL, strTypeName.GetUnicode(), NULL, NULL);
        }
    }

    // The type is marshalable or we don't care so return its size.
    rv = th.GetMethodTable()->GetNativeSize();
    HELPER_METHOD_FRAME_END();
    return rv;
}
FCIMPLEND


/************************************************************************
 * PInvoke.OffsetOfHelper(Class, Field)
 */
FCIMPL1(UINT32, MarshalNative::OffsetOfHelper, ReflectFieldObject *pFieldUNSAFE)
{
    CONTRACTL
    {
        FCALL_CHECK;
        PRECONDITION(CheckPointer(pFieldUNSAFE));
    }
    CONTRACTL_END;

    REFLECTFIELDREF refField = (REFLECTFIELDREF)ObjectToOBJECTREF(pFieldUNSAFE);

    FieldDesc *pField = refField->GetField();
    TypeHandle th = TypeHandle(pField->GetApproxEnclosingMethodTable());

    if (th.IsBlittable())
    {
        return pField->GetOffset();
    }

    UINT32 externalOffset = 0;

    HELPER_METHOD_FRAME_BEGIN_RET_1(refField);
    {
        GCX_PREEMP();
        // Determine if the type is marshalable.
        if (!IsStructMarshalable(th))
        {
            // It isn't marshalable so throw an ArgumentException.
            StackSString strTypeName;
            TypeString::AppendType(strTypeName, th);
            COMPlusThrow(kArgumentException, IDS_CANNOT_MARSHAL, strTypeName.GetUnicode(), NULL, NULL);
        }
        EEClassNativeLayoutInfo const* pNativeLayoutInfo = th.GetMethodTable()->GetNativeLayoutInfo();

        NativeFieldDescriptor const*pNFD = pNativeLayoutInfo->GetNativeFieldDescriptors();
        UINT  numReferenceFields = pNativeLayoutInfo->GetNumFields();

#ifdef _DEBUG
        bool foundField = false;
#endif
        while (numReferenceFields--)
        {
            if (pNFD->GetFieldDesc() == pField)
            {
                externalOffset = pNFD->GetExternalOffset();
                INDEBUG(foundField = true);
                break;
            }
            pNFD++;
        }

        CONSISTENCY_CHECK_MSG(foundField, "We should never hit this point since we already verified that the requested field was present from managed code");
    }
    HELPER_METHOD_FRAME_END();

    return externalOffset;
}
FCIMPLEND

FCIMPL2(Object*, MarshalNative::GetDelegateForFunctionPointerInternal, LPVOID FPtr, ReflectClassBaseObject* refTypeUNSAFE)
{
    CONTRACTL
    {
        FCALL_CHECK;
        PRECONDITION(refTypeUNSAFE != NULL);
    }
    CONTRACTL_END;

    OBJECTREF refDelegate = NULL;

    REFLECTCLASSBASEREF refType = (REFLECTCLASSBASEREF) refTypeUNSAFE;
    HELPER_METHOD_FRAME_BEGIN_RET_2(refType, refDelegate);

    // Retrieve the method table from the RuntimeType. We already verified in managed
    // code that the type was a RuntimeType that represented a delegate. Because type handles
    // for delegates must have a method table, we are safe in telling prefix to assume it below.
    MethodTable* pMT = refType->GetType().GetMethodTable();
    PREFIX_ASSUME(pMT != NULL);
    refDelegate = COMDelegate::ConvertToDelegate(FPtr, pMT);

    HELPER_METHOD_FRAME_END();

    return OBJECTREFToObject(refDelegate);
}
FCIMPLEND

FCIMPL1(LPVOID, MarshalNative::GetFunctionPointerForDelegateInternal, Object* refDelegateUNSAFE)
{
    FCALL_CONTRACT;

    LPVOID pFPtr = NULL;

    OBJECTREF refDelegate = (OBJECTREF) refDelegateUNSAFE;
    HELPER_METHOD_FRAME_BEGIN_RET_1(refDelegate);

    pFPtr = COMDelegate::ConvertToCallback(refDelegate);

    HELPER_METHOD_FRAME_END();

    return pFPtr;
}
FCIMPLEND

#ifdef _DEBUG
namespace
{
    BOOL STDMETHODCALLTYPE IsInCooperativeGCMode()
    {
        return GetThread()->PreemptiveGCDisabled();
    }
}

extern "C" IsInCooperativeGCMode_fn QCALLTYPE MarshalNative_GetIsInCooperativeGCModeFunctionPointer()
{
    QCALL_CONTRACT;

    IsInCooperativeGCMode_fn ret = NULL;

    BEGIN_QCALL;

    ret = IsInCooperativeGCMode;

    END_QCALL;

    return ret;
}
#endif

/************************************************************************
 * Marshal.GetLastPInvokeError
 */
FCIMPL0(int, MarshalNative::GetLastPInvokeError)
{
    FCALL_CONTRACT;

    return (UINT32)(GetThread()->m_dwLastError);
}
FCIMPLEND

/************************************************************************
 * Marshal.SetLastPInvokeError
 */
FCIMPL1(void, MarshalNative::SetLastPInvokeError, int error)
{
    FCALL_CONTRACT;

    GetThread()->m_dwLastError = (DWORD)error;
}
FCIMPLEND

/************************************************************************
 * Support for the GCHandle class.
 */

NOINLINE static OBJECTHANDLE FCDiagCreateHandle(OBJECTREF objRef, int type)
{
    OBJECTHANDLE hnd = NULL;

    FC_INNER_PROLOG(MarshalNative::GCHandleInternalAlloc);

    // Make the stack walkable for the profiler
    HELPER_METHOD_FRAME_BEGIN_RET_ATTRIB_NOPOLL(Frame::FRAME_ATTR_EXACT_DEPTH | Frame::FRAME_ATTR_CAPTURE_DEPTH_2);
    hnd = GetAppDomain()->CreateTypedHandle(objRef, static_cast<HandleType>(type));
    HELPER_METHOD_FRAME_END_POLL();

    FC_INNER_EPILOG();

    return hnd;
}

FCIMPL2(LPVOID, MarshalNative::GCHandleInternalAlloc, Object *obj, int type)
{
    FCALL_CONTRACT;

    OBJECTREF objRef(obj);

    assert(type >= HNDTYPE_WEAK_SHORT && type <= HNDTYPE_WEAK_NATIVE_COM);

    if (CORProfilerTrackGC())
    {
        FC_INNER_RETURN(LPVOID, (LPVOID) FCDiagCreateHandle(objRef, type));
    }

    OBJECTHANDLE hnd = GetAppDomain()->GetHandleStore()->CreateHandleOfType(OBJECTREFToObject(objRef), static_cast<HandleType>(type));
    if (!hnd)
    {
        FCThrow(kOutOfMemoryException);
    }
    return (LPVOID) hnd;
}
FCIMPLEND

NOINLINE static void FCDiagDestroyHandle(OBJECTHANDLE handle)
{
    FC_INNER_PROLOG(MarshalNative::GCHandleInternalFree);

    // Make the stack walkable for the profiler
    HELPER_METHOD_FRAME_BEGIN_ATTRIB(Frame::FRAME_ATTR_EXACT_DEPTH | Frame::FRAME_ATTR_CAPTURE_DEPTH_2);
    DestroyTypedHandle(handle);
    HELPER_METHOD_FRAME_END();

    FC_INNER_EPILOG();
}

// Free a GC handle.
FCIMPL1(VOID, MarshalNative::GCHandleInternalFree, OBJECTHANDLE handle)
{
    FCALL_CONTRACT;

    if (CORProfilerTrackGC())
    {
        FC_INNER_RETURN_VOID(FCDiagDestroyHandle(handle));
    }

    GCHandleUtilities::GetGCHandleManager()->DestroyHandleOfUnknownType(handle);
}
FCIMPLEND

// Get the object referenced by a GC handle.
FCIMPL1(LPVOID, MarshalNative::GCHandleInternalGet, OBJECTHANDLE handle)
{
    FCALL_CONTRACT;

    OBJECTREF objRef;

    objRef = ObjectFromHandle(handle);

    return *((LPVOID*)&objRef);
}
FCIMPLEND

// Update the object referenced by a GC handle.
FCIMPL2(VOID, MarshalNative::GCHandleInternalSet, OBJECTHANDLE handle, Object *obj)
{
    FCALL_CONTRACT;

    OBJECTREF objRef(obj);
    StoreObjectInHandle(handle, objRef);
}
FCIMPLEND

// Update the object referenced by a GC handle.
FCIMPL3(Object*, MarshalNative::GCHandleInternalCompareExchange, OBJECTHANDLE handle, Object *obj, Object* oldObj)
{
    FCALL_CONTRACT;

    OBJECTREF newObjref(obj);
    OBJECTREF oldObjref(oldObj);
    LPVOID ret = NULL;
    // Update the stored object reference.
    ret = InterlockedCompareExchangeObjectInHandle(handle, newObjref, oldObjref);
    return (Object*)ret;
}
FCIMPLEND

//====================================================================
// *** Interop Helpers ***
//====================================================================

FCIMPL2(Object *, MarshalNative::GetExceptionForHR, INT32 errorCode, LPVOID errorInfo)
{
    CONTRACTL
    {
        FCALL_CHECK;
        PRECONDITION(FAILED(errorCode));
        PRECONDITION(CheckPointer(errorInfo, NULL_OK));
    }
    CONTRACTL_END;

    OBJECTREF RetExceptionObj = NULL;

    HELPER_METHOD_FRAME_BEGIN_RET_1(RetExceptionObj);

    // Retrieve the IErrorInfo to use.
    IErrorInfo *pErrorInfo = (IErrorInfo*)errorInfo;
    if (pErrorInfo == (IErrorInfo*)(-1))
    {
        pErrorInfo = NULL;
    }
    else if (!pErrorInfo)
    {
        if (SafeGetErrorInfo(&pErrorInfo) != S_OK)
            pErrorInfo = NULL;
    }

    ::GetExceptionForHR(errorCode, pErrorInfo, &RetExceptionObj);

    HELPER_METHOD_FRAME_END();

    return OBJECTREFToObject(RetExceptionObj);
}
FCIMPLEND

FCIMPL1(int, MarshalNative::GetHRForException, Object* eUNSAFE)
{
    CONTRACTL {
       NOTHROW;    // Used by reverse COM IL stubs, so we must not throw exceptions back to COM
       DISABLED(GC_TRIGGERS); // FCALLS with HELPER frames have issues with GC_TRIGGERS
       MODE_COOPERATIVE;
    } CONTRACTL_END;

    int retVal = 0;
    OBJECTREF e = (OBJECTREF) eUNSAFE;
    HELPER_METHOD_FRAME_BEGIN_RET_NOTHROW_1({ retVal = COR_E_STACKOVERFLOW; }, e);

    retVal = SetupErrorInfo(e);

    HELPER_METHOD_FRAME_END_NOTHROW();
    return retVal;
}
FCIMPLEND

#ifdef FEATURE_COMINTEROP
//====================================================================
// return the IUnknown* for an Object.
//====================================================================
FCIMPL1(IUnknown*, MarshalNative::GetIUnknownForObjectNative, Object* orefUNSAFE)
{
    FCALL_CONTRACT;

    IUnknown* retVal = NULL;
    OBJECTREF oref = (OBJECTREF) orefUNSAFE;
    HELPER_METHOD_FRAME_BEGIN_RET_1(oref);

    _ASSERTE(oref != NULL);
    // Ensure COM is started up.
    EnsureComStarted();

    retVal = GetComIPFromObjectRef(&oref, ComIpType_OuterUnknown, NULL);

    HELPER_METHOD_FRAME_END();
    return retVal;
}
FCIMPLEND

//====================================================================
// return the IDispatch* for an Object.
//====================================================================
FCIMPL1(IDispatch*, MarshalNative::GetIDispatchForObjectNative, Object* orefUNSAFE)
{
    FCALL_CONTRACT;

    IDispatch* retVal = NULL;
    OBJECTREF oref = (OBJECTREF) orefUNSAFE;
    HELPER_METHOD_FRAME_BEGIN_RET_1(oref);

    _ASSERTE(oref != NULL);
    // Ensure COM is started up.
    EnsureComStarted();

    retVal = (IDispatch*)GetComIPFromObjectRef(&oref, ComIpType_Dispatch, NULL);

    HELPER_METHOD_FRAME_END();
    return retVal;
}
FCIMPLEND

//====================================================================
// return the IUnknown* representing the interface for the Object
// Object o should support Type T
//====================================================================
FCIMPL3(IUnknown*, MarshalNative::GetComInterfaceForObjectNative, Object* orefUNSAFE, ReflectClassBaseObject* refClassUNSAFE, CLR_BOOL bEnableCustomizedQueryInterface)
{
    FCALL_CONTRACT;

    IUnknown* retVal  = NULL;
    OBJECTREF oref = (OBJECTREF) orefUNSAFE;
    REFLECTCLASSBASEREF refClass = (REFLECTCLASSBASEREF) refClassUNSAFE;
    HELPER_METHOD_FRAME_BEGIN_RET_2(oref, refClass);

    _ASSERTE(oref != NULL);
    _ASSERTE(refClass != NULL);
    // Ensure COM is started up.
    EnsureComStarted();

    if (refClass->GetMethodTable() != g_pRuntimeTypeClass)
        COMPlusThrowArgumentException(W("t"), W("Argument_MustBeRuntimeType"));

    TypeHandle th = refClass->GetType();

    if (th.HasInstantiation())
        COMPlusThrowArgumentException(W("t"), W("Argument_NeedNonGenericType"));

    if (oref->GetMethodTable()->HasInstantiation())
        COMPlusThrowArgumentException(W("o"), W("Argument_NeedNonGenericObject"));

    // If the IID being asked for does not represent an interface then
    // throw an argument exception.
    if (!th.IsInterface())
        COMPlusThrowArgumentException(W("t"), W("Arg_MustBeInterface"));

    // If the interface being asked for is not visible from COM then
    // throw an argument exception.
    if (!::IsTypeVisibleFromCom(th))
        COMPlusThrowArgumentException(W("t"), W("Argument_TypeMustBeVisibleFromCom"));

    retVal = GetComIPFromObjectRef(&oref, th.GetMethodTable(), bEnableCustomizedQueryInterface);

    HELPER_METHOD_FRAME_END();
    return retVal;
}
FCIMPLEND

//====================================================================
// return an Object for IUnknown
//====================================================================
FCIMPL1(Object*, MarshalNative::GetObjectForIUnknownNative, IUnknown* pUnk)
{
    CONTRACTL
    {
        FCALL_CHECK;
        PRECONDITION(CheckPointer(pUnk));
    }
    CONTRACTL_END;

    OBJECTREF oref = NULL;
    HELPER_METHOD_FRAME_BEGIN_RET_1(oref);

    // Ensure COM is started up.
    EnsureComStarted();

    GetObjectRefFromComIP(&oref, pUnk);

    HELPER_METHOD_FRAME_END();
    return OBJECTREFToObject(oref);
}
FCIMPLEND


FCIMPL1(Object*, MarshalNative::GetUniqueObjectForIUnknownNative, IUnknown* pUnk)
{
    CONTRACTL
    {
        FCALL_CHECK;
        PRECONDITION(CheckPointer(pUnk));
    }
    CONTRACTL_END;

    OBJECTREF oref = NULL;
    HELPER_METHOD_FRAME_BEGIN_RET_1(oref);

    HRESULT hr = S_OK;

    // Ensure COM is started up.
    EnsureComStarted();

    GetObjectRefFromComIP(&oref, pUnk, NULL, ObjFromComIP::UNIQUE_OBJECT);

    HELPER_METHOD_FRAME_END();
    return OBJECTREFToObject(oref);
}
FCIMPLEND

//====================================================================
// return an Object for IUnknown, using the Type T,
//  NOTE:
//  Type T should be either a COM imported Type or a sub-type of COM imported Type
//====================================================================
FCIMPL2(Object*, MarshalNative::GetTypedObjectForIUnknown, IUnknown* pUnk, ReflectClassBaseObject* refClassUNSAFE)
{
    CONTRACTL
    {
        FCALL_CHECK;
        PRECONDITION(CheckPointer(pUnk, NULL_OK));
    }
    CONTRACTL_END;

    OBJECTREF oref = NULL;
    REFLECTCLASSBASEREF refClass = (REFLECTCLASSBASEREF) refClassUNSAFE;
    HELPER_METHOD_FRAME_BEGIN_RET_2(refClass, oref);

    HRESULT hr = S_OK;

    MethodTable* pMTClass =  NULL;

    if(!pUnk)
        COMPlusThrowArgumentNull(W("pUnk"));

    if(refClass != NULL)
    {
        if (refClass->GetMethodTable() != g_pRuntimeTypeClass)
            COMPlusThrowArgumentException(W("t"), W("Argument_MustBeRuntimeType"));

        TypeHandle th = refClass->GetType();

        if (th.HasInstantiation())
            COMPlusThrowArgumentException(W("t"), W("Argument_NeedNonGenericType"));

        pMTClass = th.GetMethodTable();
    }
    else
        COMPlusThrowArgumentNull(W("t"));


    // Ensure COM is started up.
    EnsureComStarted();

    GetObjectRefFromComIP(&oref, pUnk, pMTClass);

    HELPER_METHOD_FRAME_END();
    return OBJECTREFToObject(oref);
}
FCIMPLEND

FCIMPL2(IUnknown*, MarshalNative::CreateAggregatedObjectNative, IUnknown* pOuter, Object* refObjUNSAFE)
{
    CONTRACTL
    {
        FCALL_CHECK;
        PRECONDITION(CheckPointer(pOuter, NULL_OK));
    }
    CONTRACTL_END;

    IUnknown* pInner = NULL;

    OBJECTREF oref =  (OBJECTREF)refObjUNSAFE;
    HELPER_METHOD_FRAME_BEGIN_RET_1(oref);

    HRESULT hr = S_OK;

    if (!pOuter)
        COMPlusThrowArgumentNull(W("pOuter"));

    if (oref == NULL)
        COMPlusThrowArgumentNull(W("o"));

    // Ensure COM is started up.
    EnsureComStarted();

    if (NULL != ComCallWrapper::GetWrapperForObject(oref))
        COMPlusThrowArgumentException(W("o"), W("Argument_AlreadyACCW"));

    //get wrapper for the object, this could enable GC
    CCWHolder pWrap =  ComCallWrapper::InlineGetWrapper(&oref);

    // Aggregation support,
    pWrap->InitializeOuter(pOuter);
    IfFailThrow(pWrap->GetInnerUnknown((LPVOID*)&pInner));

    HELPER_METHOD_FRAME_END();
    return pInner;
}
FCIMPLEND

//====================================================================
// Free unused RCWs in the current COM+ context.
//====================================================================
FCIMPL0(void, MarshalNative::CleanupUnusedObjectsInCurrentContext)
{
    FCALL_CONTRACT;

    HELPER_METHOD_FRAME_BEGIN_0();

    if (g_pRCWCleanupList)
    {
        g_pRCWCleanupList->CleanupWrappersInCurrentCtxThread(
            TRUE,       // fWait
            TRUE,       // fManualCleanupRequested
            TRUE        // bIgnoreComObjectEagerCleanupSetting
            );
    }

    HELPER_METHOD_FRAME_END();
}
FCIMPLEND

//====================================================================
// Checks whether there are RCWs from any context available for cleanup.
//====================================================================
FCIMPL0(FC_BOOL_RET, MarshalNative::AreComObjectsAvailableForCleanup)
{
    FCALL_CONTRACT;

    BOOL retVal = FALSE;
    if (g_pRCWCleanupList)
    {
        retVal = !g_pRCWCleanupList->IsEmpty();
    }

    FC_RETURN_BOOL(retVal);
}
FCIMPLEND

//====================================================================
// free the COM component and zombie this object if the ref count hits 0
// further usage of this Object might throw an exception,
//====================================================================
FCIMPL1(INT32, MarshalNative::ReleaseComObject, Object* objUNSAFE)
{
    FCALL_CONTRACT;

    INT32 retVal = 0;
    OBJECTREF obj = (OBJECTREF) objUNSAFE;
    HELPER_METHOD_FRAME_BEGIN_RET_1(obj);

    if(!obj)
        COMPlusThrowArgumentNull(W("o"));

    MethodTable* pMT = obj->GetMethodTable();
    PREFIX_ASSUME(pMT != NULL);
    if(!pMT->IsComObjectType())
        COMPlusThrow(kArgumentException, IDS_EE_SRC_OBJ_NOT_COMOBJECT);

    // remove the wrapper from the object
    retVal = RCW::ExternalRelease(&obj);

    HELPER_METHOD_FRAME_END();
    return retVal;
}
FCIMPLEND

//====================================================================
// free the COM component and zombie this object
// further usage of this Object might throw an exception,
//====================================================================
FCIMPL1(void, MarshalNative::FinalReleaseComObject, Object* objUNSAFE)
{
    FCALL_CONTRACT;

    OBJECTREF obj = (OBJECTREF) objUNSAFE;
    HELPER_METHOD_FRAME_BEGIN_1(obj);

    if(!obj)
        COMPlusThrowArgumentNull(W("o"));

    MethodTable* pMT = obj->GetMethodTable();
    PREFIX_ASSUME(pMT != NULL);
    if(!pMT->IsComObjectType())
        COMPlusThrow(kArgumentException, IDS_EE_SRC_OBJ_NOT_COMOBJECT);

    // remove the wrapper from the object
    RCW::FinalExternalRelease(&obj);

    HELPER_METHOD_FRAME_END();
}
FCIMPLEND


//====================================================================
// This method takes the given COM object and wraps it in an object
// of the specified type. The type must be derived from __ComObject.
//====================================================================
FCIMPL2(Object*, MarshalNative::InternalCreateWrapperOfType, Object* objUNSAFE, ReflectClassBaseObject* refClassUNSAFE)
{
    CONTRACTL
    {
        FCALL_CHECK;
        PRECONDITION(objUNSAFE != NULL);
        PRECONDITION(refClassUNSAFE != NULL);
    }
    CONTRACTL_END;

    struct _gc
    {
        OBJECTREF refRetVal;
        OBJECTREF obj;
        REFLECTCLASSBASEREF refClass;
    } gc;

    gc.refRetVal = NULL;
    gc.obj = (OBJECTREF) objUNSAFE;
    gc.refClass = (REFLECTCLASSBASEREF) refClassUNSAFE;

    HELPER_METHOD_FRAME_BEGIN_RET_PROTECT(gc);

    // Validate the arguments.
    if (gc.refClass->GetMethodTable() != g_pRuntimeTypeClass)
        COMPlusThrowArgumentException(W("t"), W("Argument_MustBeRuntimeType"));

    // Retrieve the class of the COM object.
    MethodTable *pObjMT = gc.obj->GetMethodTable();

    // Retrieve the method table for new wrapper type.
    MethodTable *pNewWrapMT = gc.refClass->GetType().GetMethodTable();

    // Validate that the destination type is a COM object.
    _ASSERTE(pNewWrapMT->IsComObjectType());

    BOOL fSet = FALSE;

    // Start by checking if we can cast the obj to the wrapper type.
    if (TypeHandle(pObjMT).CanCastTo(TypeHandle(pNewWrapMT)))
    {
        gc.refRetVal = gc.obj;
        fSet = TRUE;
    }

    if (!fSet)
    {
        // Validate that the source object is a valid COM object.
        _ASSERTE(pObjMT->IsComObjectType());

        RCWHolder pRCW(GetThread());

        RCWPROTECT_BEGIN(pRCW, gc.obj);

        // Make sure the COM object supports all the COM imported interfaces that the new
        // wrapper class implements.
        MethodTable::InterfaceMapIterator it = pNewWrapMT->IterateInterfaceMap();
        while (it.Next())
        {
            MethodTable *pItfMT = it.GetInterfaceApprox(); // ComImport interfaces cannot be generic
            if (pItfMT->IsComImport())
            {
                if (!Object::SupportsInterface(gc.obj, pItfMT))
                    COMPlusThrow(kInvalidCastException, IDS_EE_CANNOT_COERCE_COMOBJECT);
            }
        }

        // Create the duplicate wrapper object.
        {
            RCWHolder pNewRCW(GetThread());
            pRCW->CreateDuplicateWrapper(pNewWrapMT, &pNewRCW);

            gc.refRetVal = pNewRCW->GetExposedObject();
        }

        RCWPROTECT_END(pRCW);
    }

    HELPER_METHOD_FRAME_END();
    return OBJECTREFToObject(gc.refRetVal);
}
FCIMPLEND


//====================================================================
// check if the type is visible from COM.
//====================================================================
FCIMPL1(FC_BOOL_RET, MarshalNative::IsTypeVisibleFromCom, ReflectClassBaseObject* refClassUNSAFE)
{
    FCALL_CONTRACT;

    BOOL retVal = FALSE;
    REFLECTCLASSBASEREF refClass = (REFLECTCLASSBASEREF) refClassUNSAFE;
    HELPER_METHOD_FRAME_BEGIN_RET_1(refClass);

    // Validate the arguments.
    if (refClass == NULL)
        COMPlusThrowArgumentNull(W("t"));

    MethodTable *pRefMT = refClass->GetMethodTable();
    if (pRefMT != g_pRuntimeTypeClass)
        COMPlusThrowArgumentException(W("t"), W("Argument_MustBeRuntimeType"));

    // Call the internal version of IsTypeVisibleFromCom.
    retVal = ::IsTypeVisibleFromCom(refClass->GetType());

    HELPER_METHOD_FRAME_END();
    FC_RETURN_BOOL(retVal);
}
FCIMPLEND

FCIMPL2(void, MarshalNative::GetNativeVariantForObjectNative, Object* ObjUNSAFE, LPVOID pDestNativeVariant)
{
    CONTRACTL
    {
        FCALL_CHECK;
        PRECONDITION(CheckPointer(pDestNativeVariant, NULL_OK));
    }
    CONTRACTL_END;

    OBJECTREF Obj = (OBJECTREF) ObjUNSAFE;
    HELPER_METHOD_FRAME_BEGIN_1(Obj);

    if (pDestNativeVariant == NULL)
        COMPlusThrowArgumentNull(W("pDstNativeVariant"));

    if (Obj == NULL)
    {
        // Will return empty variant in MarshalOleVariantForObject
    }
    else if (Obj->GetMethodTable()->HasInstantiation())
    {
        COMPlusThrowArgumentException(W("obj"), W("Argument_NeedNonGenericObject"));
    }

    // initialize the output variant
    SafeVariantInit((VARIANT*)pDestNativeVariant);
    OleVariant::MarshalOleVariantForObject(&Obj, (VARIANT*)pDestNativeVariant);

    HELPER_METHOD_FRAME_END();
}
FCIMPLEND

FCIMPL1(Object*, MarshalNative::GetObjectForNativeVariantNative, LPVOID pSrcNativeVariant)
{
    CONTRACTL
    {
        FCALL_CHECK;
        PRECONDITION(CheckPointer(pSrcNativeVariant, NULL_OK));
    }
    CONTRACTL_END;

    OBJECTREF Obj = NULL;
    HELPER_METHOD_FRAME_BEGIN_RET_1(Obj);

    if (pSrcNativeVariant == NULL)
        COMPlusThrowArgumentNull(W("pSrcNativeVariant"));

    OleVariant::MarshalObjectForOleVariant((VARIANT*)pSrcNativeVariant, &Obj);

    HELPER_METHOD_FRAME_END();
    return OBJECTREFToObject(Obj);
}
FCIMPLEND

FCIMPL2(Object*, MarshalNative::GetObjectsForNativeVariantsNative, VARIANT* aSrcNativeVariant, int cVars)
{
    CONTRACTL
    {
        FCALL_CHECK;
        INJECT_FAULT(FCThrow(kOutOfMemoryException););
        PRECONDITION(CheckPointer(aSrcNativeVariant, NULL_OK));
    }
    CONTRACTL_END;

    PTRARRAYREF Array = NULL;
    HELPER_METHOD_FRAME_BEGIN_RET_1(Array);

    if (aSrcNativeVariant == NULL)
        COMPlusThrowArgumentNull(W("aSrcNativeVariant"));
    if (cVars < 0)
        COMPlusThrowArgumentOutOfRange(W("cVars"), W("ArgumentOutOfRange_NeedNonNegNum"));

    OBJECTREF Obj = NULL;
    GCPROTECT_BEGIN(Obj)
    {
        // Allocate the array of objects.
        Array = (PTRARRAYREF)AllocateObjectArray(cVars, g_pObjectClass);

        // Convert each VARIANT in the array into an object.
        for (int i = 0; i < cVars; i++)
        {
            OleVariant::MarshalObjectForOleVariant(&aSrcNativeVariant[i], &Obj);
            Array->SetAt(i, Obj);
        }
    }
    GCPROTECT_END();

    HELPER_METHOD_FRAME_END();
    return OBJECTREFToObject(Array);
}
FCIMPLEND

FCIMPL1(int, MarshalNative::GetStartComSlot, ReflectClassBaseObject* tUNSAFE)
{
    FCALL_CONTRACT;

    int retVal = 0;
    REFLECTCLASSBASEREF t = (REFLECTCLASSBASEREF) tUNSAFE;
    HELPER_METHOD_FRAME_BEGIN_RET_1(t);

    if (!(t))
        COMPlusThrow(kArgumentNullException);

    MethodTable *pTMT = t->GetMethodTable();
    if (pTMT != g_pRuntimeTypeClass)
        COMPlusThrowArgumentException(W("t"), W("Argument_MustBeRuntimeType"));

    MethodTable *pMT = t->GetType().GetMethodTable();
    if (NULL == pMT)
        COMPlusThrow(kArgumentNullException);

    // The service does not make any sense to be called for non COM visible types.
    if (!::IsTypeVisibleFromCom(TypeHandle(pMT)))
        COMPlusThrowArgumentException(W("t"), W("Argument_TypeMustBeVisibleFromCom"));

    retVal = GetComSlotInfo(pMT, &pMT);

    HELPER_METHOD_FRAME_END();
    return retVal;
}
FCIMPLEND


FCIMPL1(int, MarshalNative::GetEndComSlot, ReflectClassBaseObject* tUNSAFE)
{
    FCALL_CONTRACT;

    int retVal = 0;
    REFLECTCLASSBASEREF t = (REFLECTCLASSBASEREF) tUNSAFE;
    HELPER_METHOD_FRAME_BEGIN_RET_1(t);

    int StartSlot = -1;

    if (!(t))
        COMPlusThrow(kArgumentNullException);

    MethodTable *pTMT = t->GetMethodTable();
    if (pTMT != g_pRuntimeTypeClass)
        COMPlusThrowArgumentException(W("t"), W("Argument_MustBeRuntimeType"));

    TypeHandle classTH = t->GetType();
    MethodTable *pMT = classTH.GetMethodTable();
    if (NULL == pMT)
        COMPlusThrow(kArgumentNullException);

    // The service does not make any sense to be called for non COM visible types.
    if (!::IsTypeVisibleFromCom(classTH))
        COMPlusThrowArgumentException(W("t"), W("Argument_TypeMustBeVisibleFromCom"));

    // Retrieve the start slot and the default interface class.
    StartSlot = GetComSlotInfo(pMT, &pMT);
    if (StartSlot == -1)
    {
        retVal = StartSlot;
    }
    else
    {
        // Retrieve the map of members.
        ComMTMemberInfoMap MemberMap(pMT);
        MemberMap.Init(sizeof(void*));

        // The end slot is the start slot plus the number of user defined methods.
        retVal = int(StartSlot + MemberMap.GetMethods().Size() - 1);
    }

    HELPER_METHOD_FRAME_END();
    return retVal;
}
FCIMPLEND

FCIMPL2(void, MarshalNative::ChangeWrapperHandleStrength, Object* orefUNSAFE, CLR_BOOL fIsWeak)
{
    FCALL_CONTRACT;

    OBJECTREF oref = (OBJECTREF) orefUNSAFE;
    HELPER_METHOD_FRAME_BEGIN_1(oref);

    if(oref == NULL)
        COMPlusThrowArgumentNull(W("otp"));

    if (
        !oref->GetMethodTable()->IsComImport())
    {
        CCWHolder pWrap = ComCallWrapper::InlineGetWrapper(&oref);

        if (pWrap == NULL)
            COMPlusThrowOM();
        if (fIsWeak != 0)
            pWrap->MarkHandleWeak();
        else
            pWrap->ResetHandleStrength();
    }

    HELPER_METHOD_FRAME_END();
}
FCIMPLEND

//====================================================================
// Helper function used in the COM slot to method info mapping.
//====================================================================

int MarshalNative::GetComSlotInfo(MethodTable *pMT, MethodTable **ppDefItfMT)
{
    CONTRACTL
    {
        THROWS;
        GC_TRIGGERS;
        MODE_ANY;
        PRECONDITION(CheckPointer(pMT));
        PRECONDITION(CheckPointer(ppDefItfMT));
    }
    CONTRACTL_END;

    *ppDefItfMT = NULL;

    // If a class was passed in then retrieve the default interface.
    if (!pMT->IsInterface())
    {
        TypeHandle hndDefItfClass;
        DefaultInterfaceType DefItfType = GetDefaultInterfaceForClassWrapper(TypeHandle(pMT), &hndDefItfClass);

        if (DefItfType == DefaultInterfaceType_AutoDual || DefItfType == DefaultInterfaceType_Explicit)
        {
            pMT = hndDefItfClass.GetMethodTable();
            PREFIX_ASSUME(pMT != NULL);
        }
        else
        {
            // The default interface does not have any user defined methods.
            return -1;
        }
    }

    // Set the default interface class.
    *ppDefItfMT = pMT;

    if (pMT->IsInterface())
    {
        // Return the right number of slots depending on interface type.
        return ComMethodTable::GetNumExtraSlots(pMT->GetComInterfaceType());
    }
    else
    {
        // We are dealing with an IClassX which are always IDispatch based.
        return ComMethodTable::GetNumExtraSlots(ifDispatch);
    }
}

extern "C" void QCALLTYPE MarshalNative_GetTypeFromCLSID(REFCLSID clsid, PCWSTR wszServer, QCall::ObjectHandleOnStack retType)
{
    QCALL_CONTRACT;

    BEGIN_QCALL;

    // Ensure COM is started up.
    EnsureComStarted();

    GCX_COOP();

    OBJECTREF orType = NULL;
    GCPROTECT_BEGIN(orType);
    GetComClassFromCLSID(clsid, wszServer, &orType);
    retType.Set(orType);
    GCPROTECT_END();

    END_QCALL;
}

#endif // FEATURE_COMINTEROP
