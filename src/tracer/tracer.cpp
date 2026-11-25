#include "tracer.h"
#include "clock.h"
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <sys/syscall.h>
#include <unistd.h>
#endif

constexpr uint32_t kMaxOpen = 2048;

static uint32_t CurrentOsThreadId()
{
#ifdef _WIN32
    return GetCurrentThreadId();
#else
    return static_cast<uint32_t>(syscall(SYS_gettid));
#endif
}

using ThreadEvents = std::map<uint32_t, std::vector<Event>>;

static bool IsOpening(EventType type)
{
    return type == EventType::FunctionEnter || type == EventType::UnmanagedToManaged;
}

static bool IsClosing(EventType type)
{
    return type == EventType::FunctionExit || type == EventType::FunctionTailcall || type == EventType::ManagedToUnmanaged;
}

struct OpenStack
{
    std::atomic<uint32_t> seq{0};
    std::atomic<uint32_t> depth{0};
    uint32_t dropped = 0;
    Event ev[kMaxOpen];

    void BeginWrite()
    {
        seq.store(seq.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
    }

    void EndWrite()
    {
        seq.store(seq.load(std::memory_order_relaxed) + 1, std::memory_order_release);
    }

    void Open(const Event& e)
    {
        BeginWrite();
        uint32_t d = depth.load(std::memory_order_relaxed);
        if (d < kMaxOpen)
        {
            ev[d] = e;
            depth.store(d + 1, std::memory_order_relaxed);
        }
        else
        {
            ++dropped;
        }
        EndWrite();
    }

    void Close(const Event& closing)
    {
        EventType opening = closing.type == EventType::ManagedToUnmanaged ? EventType::UnmanagedToManaged : EventType::FunctionEnter;
        BeginWrite();
        uint32_t d = depth.load(std::memory_order_relaxed);
        if (dropped > 0)
        {
            --dropped;
        }
        else
        {
            for (uint32_t i = d; i-- > 0;)
            {
                if (ev[i].type == opening && ev[i].a == closing.a)
                {
                    std::memmove(ev + i, ev + i + 1, (d - i - 1) * sizeof(Event));
                    depth.store(d - 1, std::memory_order_relaxed);
                    break;
                }
            }
        }
        EndWrite();
    }

    void Clear()
    {
        BeginWrite();
        depth.store(0, std::memory_order_relaxed);
        dropped = 0;
        EndWrite();
    }

    uint32_t Copy(Event* out) const
    {
        uint32_t n = 0;
        for (int tries = 0; tries < 16; ++tries)
        {
            uint32_t before = seq.load(std::memory_order_acquire);
            n = (std::min)(depth.load(std::memory_order_relaxed), kMaxOpen);
            std::copy_n(ev, n, out);
            std::atomic_thread_fence(std::memory_order_acquire);
            if ((before & 1) == 0 && seq.load(std::memory_order_relaxed) == before)
            {
                break;
            }
        }
        return n;
    }
};

static std::mutex g_stacksMutex;
static std::vector<OpenStack*> g_stacks;
static std::vector<OpenStack*> g_freeStacks;

struct StackGuard
{
    OpenStack* stack = nullptr;

    ~StackGuard()
    {
        if (stack != nullptr)
        {
            stack->Clear();
            std::lock_guard<std::mutex> lock(g_stacksMutex);
            g_freeStacks.push_back(stack);
            stack = nullptr;
        }
    }
};

static thread_local StackGuard tl_guard;

static OpenStack* CurrentStack()
{
    if (tl_guard.stack == nullptr)
    {
        std::lock_guard<std::mutex> lock(g_stacksMutex);
        if (!g_freeStacks.empty())
        {
            tl_guard.stack = g_freeStacks.back();
            g_freeStacks.pop_back();
        }
        else
        {
            tl_guard.stack = static_cast<OpenStack*>(std::calloc(1, sizeof(OpenStack)));
            g_stacks.push_back(tl_guard.stack);
        }
    }
    return tl_guard.stack;
}

class Tracer final : public ITracer
{
public:
    uint32_t Intern(std::string_view s)
    {
        if (s.empty())
        {
            return 0;
        }

        {
            std::shared_lock<std::shared_mutex> lock(m_stringMutex);
            auto it = m_stringIds.find(s);
            if (it != m_stringIds.end())
            {
                return it->second;
            }
        }

        std::unique_lock<std::shared_mutex> lock(m_stringMutex);
        auto it = m_stringIds.find(s);
        if (it != m_stringIds.end())
        {
            return it->second;
        }

        uint32_t id = m_stringCount.load(std::memory_order_relaxed) + 1;
        if (id > kStringChunk * kStringChunks)
        {
            return 0;
        }

        std::string* chunk = m_stringChunks[(id - 1) / kStringChunk].load(std::memory_order_relaxed);
        if (chunk == nullptr)
        {
            chunk = new std::string[kStringChunk];
            m_stringChunks[(id - 1) / kStringChunk].store(chunk, std::memory_order_relaxed);
        }

        std::string& stored = chunk[(id - 1) % kStringChunk];
        stored.assign(s);
        m_stringIds.emplace(std::string_view(stored), id);
        m_stringCount.store(id, std::memory_order_release);
        return id;
    }

    const char* GetString(uint32_t id) override
    {
        if (id == 0 || id > m_stringCount.load(std::memory_order_acquire))
        {
            return "";
        }
        return m_stringChunks[(id - 1) / kStringChunk].load(std::memory_order_relaxed)[(id - 1) % kStringChunk].c_str();
    }

    void Record(const Event& e)
    {
        if (IsOpening(e.type))
        {
            CurrentStack()->Open(e);
        }
        else if (IsClosing(e.type))
        {
            CurrentStack()->Close(e);
        }

        if (m_profiling.load(std::memory_order_acquire))
        {
            std::lock_guard<std::mutex> lock(m_profileMutex);
            if (m_profiling.load(std::memory_order_relaxed))
            {
                m_profile.push_back(e);
            }
        }
    }

    ThreadEvents TakeCrashSnapshot() override
    {
        std::vector<OpenStack*> stacks;
        {
            std::lock_guard<std::mutex> lock(g_stacksMutex);
            stacks = g_stacks;
        }

        Calib calib = MakeCalib();
        std::vector<Event> buf(kMaxOpen);
        ThreadEvents out;
        for (OpenStack* stack : stacks)
        {
            uint32_t n = stack->Copy(buf.data());
            for (uint32_t i = 0; i < n; ++i)
            {
                Event e = buf[i];
                e.timestampNs = calib.ToNs(e.timestampNs);
                out[e.osThreadId].push_back(e);
            }
        }
        return out;
    }

    void StartProfiling() override
    {
        std::lock_guard<std::mutex> lock(m_profileMutex);
        m_profiling.store(true, std::memory_order_release);
    }

    size_t TakeProfileSnapshot(Event* out, size_t capacity) override
    {
        std::lock_guard<std::mutex> lock(m_profileMutex);
        Calib calib = MakeCalib();
        size_t count = (std::min)(capacity, m_profile.size());
        for (size_t i = 0; out && i < count; ++i)
        {
            out[i] = m_profile[i];
            out[i].timestampNs = calib.ToNs(out[i].timestampNs);
        }
        return m_profile.size();
    }

    void StopProfiling() override
    {
        std::lock_guard<std::mutex> lock(m_profileMutex);
        m_profiling.store(false, std::memory_order_release);
        std::vector<Event>().swap(m_profile);
    }

    void CustomBegin(const char* name, const char* source) override
    {
        Record(Event{Ticks(), EventType::CustomBegin, CurrentOsThreadId(), 0, 0, Intern(name != nullptr ? name : ""), Intern(source != nullptr ? source : "")});
    }

    void CustomEnd(const char* name) override
    {
        Record(Event{Ticks(), EventType::CustomEnd, CurrentOsThreadId(), 0, 0, Intern(name != nullptr ? name : ""), 0});
    }

    const char* GetThreadName(uint32_t osThreadId) override
    {
        return GetString(ProfilerThreadNameId(osThreadId));
    }

    bool SetObjectTracking(bool enabled) override
    {
        return ProfilerSetObjectTracking(enabled);
    }

    bool IsObjectTracking() override
    {
        return ProfilerObjectTracking();
    }

    const char* GetFunctionName(uint64_t address) override
    {
        uint32_t nameId = 0;
        uint32_t sourceId = 0;
        return ProfilerResolveAddress(address, nameId, sourceId) ? GetString(nameId) : "";
    }

    const char* GetFunctionSource(uint64_t address) override
    {
        uint32_t nameId = 0;
        uint32_t sourceId = 0;
        return ProfilerResolveAddress(address, nameId, sourceId) ? GetString(sourceId) : "";
    }

private:
    std::atomic<bool> m_profiling{false};
    std::mutex m_profileMutex;
    std::vector<Event> m_profile;

    static constexpr uint32_t kStringChunk = 1024;
    static constexpr uint32_t kStringChunks = 1024;

    std::shared_mutex m_stringMutex;
    std::atomic<uint32_t> m_stringCount{0};
    std::atomic<std::string*> m_stringChunks[kStringChunks]{};
    std::unordered_map<std::string_view, uint32_t> m_stringIds;
};

static Tracer g_tracer;

void RecordEvent(EventType type, uint64_t a, uint64_t b, uint32_t nameId, uint32_t sourceId)
{
    g_tracer.Record(Event{Ticks(), type, CurrentOsThreadId(), a, b, nameId, sourceId});
}

uint32_t InternString(std::string_view s)
{
    return g_tracer.Intern(s);
}

#ifdef _WIN32
#define TRACER_EXPORT extern "C"
#else
#define TRACER_EXPORT extern "C" __attribute__((visibility("default")))
#endif

TRACER_EXPORT ITracer* GetTracer()
{
    return &g_tracer;
}

TRACER_EXPORT size_t TracerCrashSnapshot(Event* out, size_t capacity)
{
    size_t written = 0;
    for (auto& [threadId, events] : g_tracer.TakeCrashSnapshot())
    {
        size_t take = std::min(capacity - written, events.size());
        std::copy_n(events.begin(), take, out + written);
        written += take;
    }
    return written;
}
