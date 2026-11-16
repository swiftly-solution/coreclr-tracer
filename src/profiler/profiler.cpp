#include "../shared/extern.h"
#ifdef _WIN32
#include <windows.h>
#define IMAGE_DATA_DIRECTORY_DEFINED
#endif
#include "../stack/stack.h"
#include "../tracer/tracer.h"
#include "profiler.h"
#include <atomic>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>

static std::atomic<ICorProfilerInfo10 *> g_info{nullptr};

template <class C> static std::string ToUtf8(const C *s, size_t n) {
  std::string out;
  for (size_t i = 0; i < n; ++i) {
    uint32_t c = static_cast<uint32_t>(s[i]);
    if (sizeof(C) == 2 && c >= 0xD800 && c < 0xDC00 && i + 1 < n) {
      uint32_t lo = static_cast<uint32_t>(s[i + 1]);
      if (lo >= 0xDC00 && lo < 0xE000) {
        c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
        ++i;
      }
    }
    if (c < 0x80) {
      out += static_cast<char>(c);
    } else if (c < 0x800) {
      out += static_cast<char>(0xC0 | (c >> 6));
      out += static_cast<char>(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
      out += static_cast<char>(0xE0 | (c >> 12));
      out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (c & 0x3F));
    } else {
      out += static_cast<char>(0xF0 | (c >> 18));
      out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
      out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (c & 0x3F));
    }
  }
  return out;
}

static std::string ToUtf8(const std::wstring &w) {
  return ToUtf8(w.data(), w.size());
}

static std::string ResolveModuleName(ICorProfilerInfo10 *info, ModuleID id) {
  std::vector<WCHAR> buf(4096);
  ULONG n = 0;
  if (FAILED(info->GetModuleInfo(id, nullptr, static_cast<ULONG>(buf.size()),
                                 &n, buf.data(), nullptr))) {
    return {};
  }
  return ToUtf8(buf.data(), n > 0 ? n - 1 : 0);
}

struct FnSym {
  uint64_t addr;
  uint32_t nameId;
  uint32_t moduleNameId;
};
static std::shared_mutex g_symMutex;
static std::unordered_map<FunctionID, FnSym> g_functions;
static std::unordered_map<ClassID, uint32_t> g_classes;

static FnSym ResolveFunction(FunctionID id) {
  {
    std::shared_lock<std::shared_mutex> lock(g_symMutex);
    auto it = g_functions.find(id);
    if (it != g_functions.end()) {
      return it->second;
    }
  }

  ICorProfilerInfo10 *info = g_info.load();
  if (info == nullptr) {
    return FnSym{};
  }

  FnSym sym{};
  FunctionInfo fn;
  if (SUCCEEDED(GetFunctionInfo(info, id, fn))) {
    sym.nameId = InternString(ToUtf8(GetFunctionFullName(fn)));
    sym.moduleNameId = InternString(ResolveModuleName(info, fn.moduleId));
  }
  ULONG32 count = 0;
  COR_PRF_CODE_INFO code[1] = {};
  if (SUCCEEDED(info->GetCodeInfo2(id, 1, &count, code)) && count > 0) {
    sym.addr = static_cast<uint64_t>(code[0].startAddress);
  }

  std::unique_lock<std::shared_mutex> lock(g_symMutex);
  return g_functions.emplace(id, sym).first->second;
}

static uint32_t ResolveClassName(ClassID id) {
  {
    std::shared_lock<std::shared_mutex> lock(g_symMutex);
    auto it = g_classes.find(id);
    if (it != g_classes.end()) {
      return it->second;
    }
  }

  ICorProfilerInfo10 *info = g_info.load();
  if (info == nullptr) {
    return 0;
  }

  uint32_t nameId = InternString(ToUtf8(GetClassName(info, id)));
  std::unique_lock<std::shared_mutex> lock(g_symMutex);
  return g_classes.emplace(id, nameId).first->second;
}

struct ThreadSym {
  uint32_t nameId;
  uint32_t osId;
};
static std::mutex g_threadMutex;
static std::unordered_map<ThreadID, ThreadSym> g_threads;

static constexpr uint32_t kShadowMax = 1024;
static thread_local FunctionID tl_stack[kShadowMax];
static thread_local uint32_t tl_depth = 0;

static void ShadowPush(FunctionID id) {
  if (tl_depth < kShadowMax) {
    tl_stack[tl_depth] = id;
  }
  ++tl_depth;
}

static void ShadowPop() {
  if (tl_depth > 0) {
    --tl_depth;
  }
}

static FunctionID ShadowTop() {
  return tl_depth > 0 && tl_depth <= kShadowMax ? tl_stack[tl_depth - 1] : 0;
}

PROFILER_STUB EnterStub(FunctionIDOrClientID functionId,
                        COR_PRF_ELT_INFO eltInfo) {
  FunctionID id = functionId.functionID;
  FnSym sym = ResolveFunction(id);
  RecordEvent(EventType::FunctionEnter, id, sym.addr, sym.nameId,
              sym.moduleNameId);
  ShadowPush(id);
}

PROFILER_STUB ExitStub(FunctionIDOrClientID functionId,
                       COR_PRF_ELT_INFO eltInfo) {
  FunctionID id = functionId.functionID;
  FnSym sym = ResolveFunction(id);
  RecordEvent(EventType::FunctionExit, id, sym.addr, sym.nameId,
              sym.moduleNameId);
  ShadowPop();
}

PROFILER_STUB TailcallStub(FunctionIDOrClientID functionId,
                           COR_PRF_ELT_INFO eltInfo) {
  FunctionID id = functionId.functionID;
  FnSym sym = ResolveFunction(id);
  RecordEvent(EventType::FunctionTailcall, id, sym.addr, sym.nameId,
              sym.moduleNameId);
  ShadowPop();
}

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
  g_info.store(this->m_pCoreProfilerInfo);

  DWORD eventMask = COR_PRF_MONITOR_ENTERLEAVE | COR_PRF_ENABLE_FUNCTION_ARGS |
                    COR_PRF_MONITOR_THREADS | COR_PRF_MONITOR_CODE_TRANSITIONS |
                    COR_PRF_ENABLE_FUNCTION_RETVAL | COR_PRF_ENABLE_FRAME_INFO |
                    COR_PRF_MONITOR_OBJECT_ALLOCATED |
                    COR_PRF_ENABLE_OBJECT_ALLOCATED |
                    COR_PRF_MONITOR_EXCEPTIONS;

  auto hr = this->m_pCoreProfilerInfo->SetEventMask(eventMask);
  if (hr != S_OK) {
    return E_FAIL;
  }

  hr = this->m_pCoreProfilerInfo->SetEnterLeaveFunctionHooks3WithInfo(
      EnterStub, ExitStub, TailcallStub);
  return hr;
}

HRESULT STDMETHODCALLTYPE CorProfiler::Shutdown() {
  g_info.store(nullptr);
  if (this->m_pCoreProfilerInfo != nullptr) {
    this->m_pCoreProfilerInfo->Release();
    this->m_pCoreProfilerInfo = nullptr;
  }

  return S_OK;
}

HRESULT STDMETHODCALLTYPE CorProfiler::ThreadCreated(ThreadID threadId) {
  {
    std::lock_guard<std::mutex> lock(g_threadMutex);
    g_threads[threadId] = ThreadSym{};
  }
  RecordEvent(EventType::ThreadCreated, threadId);
  return S_OK;
}

HRESULT STDMETHODCALLTYPE CorProfiler::ThreadDestroyed(ThreadID threadId) {
  ThreadSym t{};
  {
    std::lock_guard<std::mutex> lock(g_threadMutex);
    auto it = g_threads.find(threadId);
    if (it != g_threads.end()) {
      t = it->second;
      g_threads.erase(it);
    }
  }
  RecordEvent(EventType::ThreadDestroyed, threadId, t.osId, t.nameId);
  return S_OK;
}

HRESULT STDMETHODCALLTYPE CorProfiler::ThreadAssignedToOSThread(
    ThreadID managedThreadId, DWORD osThreadId) {
  uint32_t nameId;
  {
    std::lock_guard<std::mutex> lock(g_threadMutex);
    ThreadSym &t = g_threads[managedThreadId];
    t.osId = osThreadId;
    nameId = t.nameId;
  }
  RecordEvent(EventType::ThreadAssignedToOSThread, managedThreadId, osThreadId,
              nameId);
  return S_OK;
}

HRESULT STDMETHODCALLTYPE CorProfiler::ThreadNameChanged(ThreadID threadId,
                                                         ULONG cchName,
                                                         WCHAR name[]) {
  while (name != nullptr && cchName > 0 && name[cchName - 1] == 0) {
    --cchName;
  }
  uint32_t nameId = name != nullptr ? InternString(ToUtf8(name, cchName)) : 0;
  uint32_t osId;
  {
    std::lock_guard<std::mutex> lock(g_threadMutex);
    ThreadSym &t = g_threads[threadId];
    t.nameId = nameId;
    osId = t.osId;
  }
  RecordEvent(EventType::ThreadNameChanged, threadId, osId, nameId);
  return S_OK;
}

HRESULT STDMETHODCALLTYPE CorProfiler::UnmanagedToManagedTransition(
    FunctionID functionId, COR_PRF_TRANSITION_REASON reason) {
  RecordEvent(EventType::UnmanagedToManaged, functionId, reason,
              ResolveFunction(functionId).nameId);
  return S_OK;
}

HRESULT STDMETHODCALLTYPE CorProfiler::ObjectAllocated(ObjectID objectId,
                                                       ClassID classId) {
  SIZE_T size = 0;
  this->m_pCoreProfilerInfo->GetObjectSize2(objectId, &size);
  FunctionID source = ShadowTop();
  RecordEvent(EventType::ObjectAllocated, objectId, size,
              ResolveClassName(classId),
              source != 0 ? ResolveFunction(source).nameId : 0);
  return S_OK;
}

HRESULT STDMETHODCALLTYPE CorProfiler::ExceptionUnwindFunctionLeave(void) {
  ShadowPop();
  return S_OK;
}
