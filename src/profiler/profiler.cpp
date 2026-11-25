#include "../shared/extern.h"
#ifdef _WIN32
#include <windows.h>
#define IMAGE_DATA_DIRECTORY_DEFINED
#endif
#include "../stack/stack.h"
#include "../tracer/tracer.h"
#include "profiler.h"
#include <atomic>
#include <map>
#include <mutex>
#include <vector>
#include <shared_mutex>
#include <string>
#include <unordered_map>

static std::atomic<ICorProfilerInfo10*> g_info{nullptr};

template <class C> static std::string ToUtf8(const C* s, size_t n)
{
    std::string out;
    for (size_t i = 0; i < n; ++i)
    {
        uint32_t c = static_cast<uint32_t>(s[i]);
        if (sizeof(C) == 2 && c >= 0xD800 && c < 0xDC00 && i + 1 < n)
        {
            uint32_t lo = static_cast<uint32_t>(s[i + 1]);
            if (lo >= 0xDC00 && lo < 0xE000)
            {
                c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
                ++i;
            }
        }

        if (c < 0x80)
        {
            out += static_cast<char>(c);
        }
        else if (c < 0x800)
        {
            out += static_cast<char>(0xC0 | (c >> 6));
            out += static_cast<char>(0x80 | (c & 0x3F));
        }
        else if (c < 0x10000)
        {
            out += static_cast<char>(0xE0 | (c >> 12));
            out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (c & 0x3F));
        }
        else
        {
            out += static_cast<char>(0xF0 | (c >> 18));
            out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (c & 0x3F));
        }
    }
    return out;
}

static std::string ToUtf8(const std::wstring& w)
{
    return ToUtf8(w.data(), w.size());
}

static std::string ResolveModuleName(ICorProfilerInfo10* info, ModuleID id)
{
    std::vector<WCHAR> buf(4096);
    ULONG n = 0;
    if (FAILED(info->GetModuleInfo(id, nullptr, static_cast<ULONG>(buf.size()), &n, buf.data(), nullptr)))
    {
        return {};
    }
    return ToUtf8(buf.data(), n > 0 ? n - 1 : 0);
}

static bool IsHookedModule(ICorProfilerInfo10* info, ModuleID moduleId)
{
    static const char* const kSkipped[] = {"System.", "Microsoft.", "mscorlib", "netstandard"};

    std::string path = ResolveModuleName(info, moduleId);
    size_t slash = path.find_last_of("\\/");
    std::string file = slash == std::string::npos ? path : path.substr(slash + 1);
    for (const char* prefix : kSkipped)
    {
        if (file.rfind(prefix, 0) == 0)
        {
            return false;
        }
    }
    return true;
}

struct FnSym
{
    uint64_t addr;
    uint32_t nameId;
    uint32_t moduleNameId;
};

struct FnEntry
{
    explicit FnEntry(FunctionID functionId) : id(functionId) {}

    FunctionID id;
    std::atomic<bool> resolved{false};
    FnSym sym{};
};

static std::shared_mutex g_symMutex;
static std::unordered_map<FunctionID, FnEntry*> g_functions;
static std::unordered_map<ClassID, uint32_t> g_classes;
static std::mutex g_resolveMutex;

static FnSym QuerySymbols(FunctionID id)
{
    FnSym sym{};
    ICorProfilerInfo10* info = g_info.load();
    if (info == nullptr)
    {
        return sym;
    }

    FunctionInfo fn;
    if (SUCCEEDED(GetFunctionInfo(info, id, fn)))
    {
        sym.nameId = InternString(ToUtf8(GetFunctionFullName(fn)));
        sym.moduleNameId = InternString(ResolveModuleName(info, fn.moduleId));
    }

    ULONG32 count = 0;
    COR_PRF_CODE_INFO code[1] = {};
    if (SUCCEEDED(info->GetCodeInfo2(id, 1, &count, code)) && count > 0)
    {
        sym.addr = static_cast<uint64_t>(code[0].startAddress);
    }
    return sym;
}

static const FnSym& ResolveEntrySlow(FnEntry* e)
{
    std::lock_guard<std::mutex> lock(g_resolveMutex);
    if (!e->resolved.load(std::memory_order_relaxed))
    {
        e->sym = QuerySymbols(e->id);
        if (e->sym.nameId != 0)
        {
            e->resolved.store(true, std::memory_order_release);
        }
    }
    return e->sym;
}

static inline const FnSym& ResolveEntry(FnEntry* e)
{
    return e->resolved.load(std::memory_order_acquire) ? e->sym : ResolveEntrySlow(e);
}

static FnSym ResolveFunction(FunctionID id)
{
    FnEntry* e = nullptr;
    {
        std::shared_lock<std::shared_mutex> lock(g_symMutex);
        auto it = g_functions.find(id);
        if (it != g_functions.end())
        {
            e = it->second;
        }
    }
    return e != nullptr ? ResolveEntry(e) : QuerySymbols(id);
}

static uint32_t ResolveClassNameShared(ClassID id)
{
    {
        std::shared_lock<std::shared_mutex> lock(g_symMutex);
        auto it = g_classes.find(id);
        if (it != g_classes.end())
        {
            return it->second;
        }
    }

    ICorProfilerInfo10* info = g_info.load();
    if (info == nullptr)
    {
        return 0;
    }

    uint32_t nameId = InternString(ToUtf8(GetClassName(info, id)));
    std::unique_lock<std::shared_mutex> lock(g_symMutex);
    return g_classes.emplace(id, nameId).first->second;
}

struct ClassCacheEntry
{
    ClassID id;
    uint32_t nameId;
};

static thread_local ClassCacheEntry tl_classes[64];

static uint32_t ResolveClassName(ClassID id)
{
    ClassCacheEntry& cached = tl_classes[(id >> 4) & 63];
    if (id != 0 && cached.id == id)
    {
        return cached.nameId;
    }

    uint32_t nameId = ResolveClassNameShared(id);
    cached = ClassCacheEntry{id, nameId};
    return nameId;
}

static void RecordFunction(EventType type, FunctionID id, const FnSym& sym)
{
    RecordEvent(type, id, sym.addr, sym.nameId, sym.moduleNameId);
}

struct CodeRange
{
    uint64_t size;
    uint32_t nameId;
    uint32_t sourceId;
    ModuleID module;
};
static std::shared_mutex g_codeMutex;
static std::map<uint64_t, CodeRange> g_codeRanges;

static void RegisterJitCode(ICorProfilerInfo10* info, FunctionID id)
{
    FunctionInfo fn;
    if (FAILED(GetFunctionInfo(info, id, fn)) || !IsHookedModule(info, fn.moduleId))
    {
        return;
    }
    uint32_t nameId = InternString(ToUtf8(GetFunctionFullName(fn)));
    uint32_t sourceId = InternString(ResolveModuleName(info, fn.moduleId));

    std::vector<COR_PRF_CODE_INFO> chunks(4);
    ULONG32 count = 0;
    HRESULT hr = info->GetCodeInfo2(id, static_cast<ULONG32>(chunks.size()), &count, chunks.data());
    if (count > chunks.size())
    {
        chunks.resize(count);
        hr = info->GetCodeInfo2(id, count, &count, chunks.data());
    }
    if (FAILED(hr))
    {
        return;
    }

    std::unique_lock<std::shared_mutex> lock(g_codeMutex);
    for (ULONG32 i = 0; i < count; ++i)
    {
        g_codeRanges[chunks[i].startAddress] = CodeRange{chunks[i].size, nameId, sourceId, fn.moduleId};
    }
}

static void UnregisterModuleCode(ModuleID module)
{
    std::unique_lock<std::shared_mutex> lock(g_codeMutex);
    for (auto it = g_codeRanges.begin(); it != g_codeRanges.end();)
    {
        it = it->second.module == module ? g_codeRanges.erase(it) : std::next(it);
    }
}

bool ProfilerResolveAddress(uint64_t address, uint32_t& nameId, uint32_t& sourceId)
{
    std::shared_lock<std::shared_mutex> lock(g_codeMutex, std::try_to_lock);
    if (!lock.owns_lock())
    {
        return false;
    }
    auto it = g_codeRanges.upper_bound(address);
    if (it == g_codeRanges.begin())
    {
        return false;
    }
    --it;
    if (address - it->first >= it->second.size)
    {
        return false;
    }
    nameId = it->second.nameId;
    sourceId = it->second.sourceId;
    return true;
}

static UINT_PTR STDMETHODCALLTYPE FunctionMapper(FunctionID id, void*, BOOL* hook)
{
    FunctionInfo fn;
    ICorProfilerInfo10* info = g_info.load();
    if (info == nullptr || FAILED(GetFunctionInfo(info, id, fn)) || !IsHookedModule(info, fn.moduleId))
    {
        *hook = FALSE;
        return 0;
    }

    *hook = TRUE;
    std::unique_lock<std::shared_mutex> lock(g_symMutex);
    FnEntry*& e = g_functions[id];
    if (e == nullptr)
    {
        e = new FnEntry(id);
    }
    return reinterpret_cast<UINT_PTR>(e);
}

struct ThreadSym
{
    uint32_t nameId;
    uint32_t osId;
};

static std::mutex g_threadMutex;
static std::unordered_map<ThreadID, ThreadSym> g_threads;

uint32_t ProfilerThreadNameId(uint32_t osThreadId)
{
    if (osThreadId == 0)
    {
        return 0;
    }
    std::lock_guard<std::mutex> lock(g_threadMutex);
    for (const auto& entry : g_threads)
    {
        if (entry.second.osId == osThreadId)
        {
            return entry.second.nameId;
        }
    }
    return 0;
}

static constexpr uint32_t kShadowMax = 1024;
static thread_local FnEntry* tl_stack[kShadowMax];
static thread_local uint32_t tl_depth = 0;
static thread_local bool tl_unwindPop = false;

static void ShadowPush(FnEntry* e)
{
    if (tl_depth < kShadowMax)
    {
        tl_stack[tl_depth] = e;
    }
    ++tl_depth;
}

static void ShadowPop()
{
    if (tl_depth > 0)
    {
        --tl_depth;
    }
}

static FnEntry* ShadowTop()
{
    return tl_depth > 0 && tl_depth <= kShadowMax ? tl_stack[tl_depth - 1] : nullptr;
}

static FnEntry* ToEntry(FunctionIDOrClientID functionId)
{
    return reinterpret_cast<FnEntry*>(functionId.clientID);
}

static void RecordEntry(EventType type, FnEntry* e)
{
    RecordFunction(type, e->id, ResolveEntry(e));
}

static std::mutex g_maskMutex;
static std::atomic<bool> g_objectTracking{false};

PROFILER_STUB EnterStub(FunctionIDOrClientID functionId, COR_PRF_ELT_INFO)
{
    FnEntry* e = ToEntry(functionId);
    RecordEntry(EventType::FunctionEnter, e);
    ShadowPush(e);
}

PROFILER_STUB ExitStub(FunctionIDOrClientID functionId, COR_PRF_ELT_INFO)
{
    RecordEntry(EventType::FunctionExit, ToEntry(functionId));
    ShadowPop();
}

PROFILER_STUB TailcallStub(FunctionIDOrClientID functionId, COR_PRF_ELT_INFO)
{
    RecordEntry(EventType::FunctionTailcall, ToEntry(functionId));
    ShadowPop();
}

bool ProfilerSetObjectTracking(bool enabled)
{
    std::lock_guard<std::mutex> lock(g_maskMutex);
    ICorProfilerInfo10* info = g_info.load();
    DWORD mask = 0;
    if (info == nullptr || FAILED(info->GetEventMask(&mask)))
    {
        return false;
    }

    constexpr DWORD kFlag = COR_PRF_MONITOR_OBJECT_ALLOCATED;
    mask = enabled ? (mask | kFlag) : (mask & ~kFlag);
    if (FAILED(info->SetEventMask(mask)))
    {
        return false;
    }

    g_objectTracking.store(enabled);
    return true;
}

bool ProfilerObjectTracking()
{
    return g_objectTracking.load();
}

CorProfiler::CorProfiler() : m_iRefCount(0), m_pCoreProfilerInfo(nullptr) {}

CorProfiler::~CorProfiler()
{
    Shutdown();
}

HRESULT STDMETHODCALLTYPE CorProfiler::Initialize(IUnknown* pICorProfilerInfoUnk)
{
    if (FAILED(pICorProfilerInfoUnk->QueryInterface(__uuidof(ICorProfilerInfo10), reinterpret_cast<void**>(&m_pCoreProfilerInfo))))
    {
        return E_FAIL;
    }
    g_info.store(m_pCoreProfilerInfo);

    constexpr DWORD kEventMask = COR_PRF_MONITOR_ENTERLEAVE | COR_PRF_ENABLE_FUNCTION_ARGS | COR_PRF_MONITOR_THREADS | COR_PRF_MONITOR_CODE_TRANSITIONS | COR_PRF_MONITOR_EXCEPTIONS | COR_PRF_MONITOR_JIT_COMPILATION | COR_PRF_MONITOR_MODULE_LOADS /*| COR_PRF_ENABLE_OBJECT_ALLOCATED*/;

    if (m_pCoreProfilerInfo->SetEventMask(kEventMask) != S_OK || FAILED(m_pCoreProfilerInfo->SetFunctionIDMapper2(FunctionMapper, nullptr)))
    {
        return E_FAIL;
    }

    return m_pCoreProfilerInfo->SetEnterLeaveFunctionHooks3WithInfo(EnterStub, ExitStub, TailcallStub);
}

HRESULT STDMETHODCALLTYPE CorProfiler::Shutdown()
{
    g_info.store(nullptr);
    if (m_pCoreProfilerInfo != nullptr)
    {
        m_pCoreProfilerInfo->Release();
        m_pCoreProfilerInfo = nullptr;
    }
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CorProfiler::ThreadCreated(ThreadID threadId)
{
    {
        std::lock_guard<std::mutex> lock(g_threadMutex);
        g_threads.try_emplace(threadId);
    }
    RecordEvent(EventType::ThreadCreated, threadId);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CorProfiler::ThreadDestroyed(ThreadID threadId)
{
    ThreadSym t{};
    {
        std::lock_guard<std::mutex> lock(g_threadMutex);
        auto it = g_threads.find(threadId);
        if (it != g_threads.end())
        {
            t = it->second;
            g_threads.erase(it);
        }
    }
    RecordEvent(EventType::ThreadDestroyed, threadId, t.osId, t.nameId);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CorProfiler::ThreadAssignedToOSThread(ThreadID managedThreadId, DWORD osThreadId)
{
    uint32_t nameId;
    {
        std::lock_guard<std::mutex> lock(g_threadMutex);
        ThreadSym& t = g_threads[managedThreadId];
        t.osId = osThreadId;
        nameId = t.nameId;
    }
    RecordEvent(EventType::ThreadAssignedToOSThread, managedThreadId, osThreadId, nameId);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CorProfiler::ThreadNameChanged(ThreadID threadId, ULONG cchName, WCHAR name[])
{
    while (name != nullptr && cchName > 0 && name[cchName - 1] == 0)
    {
        --cchName;
    }

    uint32_t nameId = name != nullptr ? InternString(ToUtf8(name, cchName)) : 0;
    uint32_t osId;
    {
        std::lock_guard<std::mutex> lock(g_threadMutex);
        ThreadSym& t = g_threads[threadId];
        t.nameId = nameId;
        osId = t.osId;
    }
    RecordEvent(EventType::ThreadNameChanged, threadId, osId, nameId);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CorProfiler::UnmanagedToManagedTransition(FunctionID functionId, COR_PRF_TRANSITION_REASON reason)
{
    if (reason == COR_PRF_TRANSITION_CALL)
    {
        RecordFunction(EventType::UnmanagedToManaged, functionId, ResolveFunction(functionId));
    }
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CorProfiler::ManagedToUnmanagedTransition(FunctionID functionId, COR_PRF_TRANSITION_REASON reason)
{
    if (reason == COR_PRF_TRANSITION_RETURN && functionId != 0)
    {
        RecordFunction(EventType::ManagedToUnmanaged, functionId, ResolveFunction(functionId));
    }
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CorProfiler::ModuleUnloadStarted(ModuleID moduleId)
{
    UnregisterModuleCode(moduleId);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CorProfiler::JITCompilationFinished(FunctionID functionId, HRESULT hrStatus, BOOL fIsSafeToBlock)
{
    ICorProfilerInfo10* info = g_info.load();
    if (SUCCEEDED(hrStatus) && info != nullptr)
    {
        RegisterJitCode(info, functionId);
    }
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CorProfiler::ObjectAllocated(ObjectID objectId, ClassID classId)
{
    if (!g_objectTracking.load(std::memory_order_relaxed))
    {
        return S_OK;
    }

    SIZE_T size = 0;
    m_pCoreProfilerInfo->GetObjectSize2(objectId, &size);
    FnEntry* source = ShadowTop();
    RecordEvent(EventType::ObjectAllocated, objectId, size, ResolveClassName(classId), source != nullptr ? ResolveEntry(source).nameId : 0);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CorProfiler::ExceptionUnwindFunctionEnter(FunctionID functionId)
{
    FnEntry* top = ShadowTop();
    tl_unwindPop = top != nullptr && top->id == functionId;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CorProfiler::ExceptionUnwindFunctionLeave()
{
    if (!tl_unwindPop)
    {
        return S_OK;
    }

    tl_unwindPop = false;
    if (FnEntry* e = ShadowTop())
    {
        RecordEntry(EventType::FunctionExit, e);
    }
    ShadowPop();
    return S_OK;
}
