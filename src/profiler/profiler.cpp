#include "../shared/extern.h"
#ifdef _WIN32
#include <windows.h>
#define IMAGE_DATA_DIRECTORY_DEFINED
#endif
#include "../stack/stack.h"
#include "profiler.h"
#include <cstdio>
#include <cwchar>

PROFILER_STUB EnterStub(FunctionIDOrClientID functionId,
                        COR_PRF_ELT_INFO eltInfo) {}

PROFILER_STUB ExitStub(FunctionIDOrClientID functionId,
                       COR_PRF_ELT_INFO eltInfo) {}

PROFILER_STUB TailcallStub(FunctionIDOrClientID functionId,
                           COR_PRF_ELT_INFO eltInfo) {}

EXTERN_C void EnterNaked(FunctionIDOrClientID functionIDOrClientID,
                         COR_PRF_ELT_INFO eltInfo);
EXTERN_C void ExitNaked(FunctionIDOrClientID functionIDOrClientID,
                        COR_PRF_ELT_INFO eltInfo);
EXTERN_C void TailcallNaked(FunctionIDOrClientID functionIDOrClientID,
                            COR_PRF_ELT_INFO eltInfo);

CorProfiler::CorProfiler() : m_iRefCount(0), m_pCoreProfilerInfo(nullptr) {}

CorProfiler::~CorProfiler() {
  if (this->m_pCoreProfilerInfo != nullptr) {
    this->m_pCoreProfilerInfo->Release();
    this->m_pCoreProfilerInfo = nullptr;
  }
}

HRESULT STDMETHODCALLTYPE
CorProfiler::Initialize(IUnknown *pICorProfilerInfoUnk) {
  HRESULT queryInterfaceResult = pICorProfilerInfoUnk->QueryInterface(
      __uuidof(ICorProfilerInfo10),
      reinterpret_cast<void **>(&this->m_pCoreProfilerInfo));

  if (FAILED(queryInterfaceResult)) {
    return E_FAIL;
  }

  DWORD eventMask = COR_PRF_MONITOR_ENTERLEAVE | COR_PRF_ENABLE_FUNCTION_ARGS |
                    COR_PRF_MONITOR_THREADS | COR_PRF_MONITOR_CODE_TRANSITIONS |
                    COR_PRF_ENABLE_FUNCTION_RETVAL | COR_PRF_ENABLE_FRAME_INFO |
                    COR_PRF_MONITOR_OBJECT_ALLOCATED |
                    COR_PRF_ENABLE_OBJECT_ALLOCATED;

  auto hr = this->m_pCoreProfilerInfo->SetEventMask(eventMask);
  if (hr != S_OK) {
    return E_FAIL;
  }

  hr = this->m_pCoreProfilerInfo->SetEnterLeaveFunctionHooks3WithInfo(
      EnterNaked, ExitNaked, TailcallNaked);
  return hr;
}

HRESULT STDMETHODCALLTYPE CorProfiler::Shutdown() {
  if (this->m_pCoreProfilerInfo != nullptr) {
    this->m_pCoreProfilerInfo->Release();
    this->m_pCoreProfilerInfo = nullptr;
  }

  return S_OK;
}

HRESULT STDMETHODCALLTYPE CorProfiler::ThreadCreated(ThreadID threadId) {
  return S_OK;
}

HRESULT STDMETHODCALLTYPE CorProfiler::ThreadDestroyed(ThreadID threadId) {
  return S_OK;
}

HRESULT STDMETHODCALLTYPE CorProfiler::ThreadAssignedToOSThread(
    ThreadID managedThreadId, DWORD osThreadId) {
  return S_OK;
}

HRESULT STDMETHODCALLTYPE CorProfiler::UnmanagedToManagedTransition(
    FunctionID functionId, COR_PRF_TRANSITION_REASON reason) {
  return S_OK;
}

HRESULT STDMETHODCALLTYPE CorProfiler::ObjectAllocated(ObjectID objectId,
                                                       ClassID classId) {
  return S_OK;
}

std::wstring CorProfiler::GetCachedClassName(ClassID classId) {
  {
    std::lock_guard<std::mutex> lock(m_classNamesMutex);
    auto it = m_classNames.find(classId);
    if (it != m_classNames.end()) {
      return it->second;
    }
  }

  std::wstring name = GetClassName(m_pCoreProfilerInfo, classId);

  std::lock_guard<std::mutex> lock(m_classNamesMutex);
  m_classNames.emplace(classId, name);
  return name;
}
