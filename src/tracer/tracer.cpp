#include "tracer.h"
#include "clock.h"
#include <algorithm>
#include <atomic>
#include <cstdlib>
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

constexpr size_t kRingSize = 1 << 14;
constexpr size_t kRingMask = kRingSize - 1;

static uint32_t CurrentOsThreadId()
{
#ifdef _WIN32
    return GetCurrentThreadId();
#else
    return static_cast<uint32_t>(syscall(SYS_gettid));
#endif
}

struct Slot
{
    std::atomic<uint64_t> seq{0};
    Event ev{};
};

struct Ring
{
    std::atomic<uint64_t> head{0};
    Slot slots[kRingSize];

    void Push(const Event& e)
    {
        uint64_t ticket = head.load(std::memory_order_relaxed);
        Slot& slot = slots[ticket & kRingMask];
        slot.seq.store(2 * ticket + 1, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        slot.ev = e;
        slot.seq.store(2 * ticket + 2, std::memory_order_release);
        head.store(ticket + 1, std::memory_order_release);
    }

    bool Read(uint64_t ticket, Event& out) const
    {
        const Slot& slot = slots[ticket & kRingMask];
        uint64_t before = slot.seq.load(std::memory_order_acquire);
        if (before != 2 * ticket + 2)
        {
            return false;
        }
        out = slot.ev;
        std::atomic_thread_fence(std::memory_order_acquire);
        return slot.seq.load(std::memory_order_relaxed) == before;
    }
};

static std::mutex g_ringsMutex;
static std::vector<Ring*> g_rings;
static std::vector<Ring*> g_freeRings;

struct RingGuard
{
    Ring* ring = nullptr;

    ~RingGuard()
    {
        if (ring != nullptr)
        {
            std::lock_guard<std::mutex> lock(g_ringsMutex);
            g_freeRings.push_back(ring);
            ring = nullptr;
        }
    }
};

static thread_local RingGuard tl_guard;

static Ring* CurrentRing()
{
    if (tl_guard.ring == nullptr)
    {
        std::lock_guard<std::mutex> lock(g_ringsMutex);
        if (!g_freeRings.empty())
        {
            tl_guard.ring = g_freeRings.back();
            g_freeRings.pop_back();
        }
        else
        {
            tl_guard.ring = static_cast<Ring*>(std::calloc(1, sizeof(Ring)));
            g_rings.push_back(tl_guard.ring);
        }
    }
    return tl_guard.ring;
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

static void CloseOpenEvent(ThreadEvents& events, const Event& closing)
{
    EventType opening = closing.type == EventType::ManagedToUnmanaged ? EventType::UnmanagedToManaged : EventType::FunctionEnter;
    auto it = events.find(closing.osThreadId);
    if (it == events.end())
    {
        return;
    }

    auto& open = it->second;
    for (size_t i = open.size(); i-- > 0;)
    {
        if (open[i].type == opening && open[i].a == closing.a)
        {
            open.erase(open.begin() + i);
            break;
        }
    }
    if (open.empty())
    {
        events.erase(it);
    }
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
        CurrentRing()->Push(e);

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
        std::vector<Ring*> rings;
        {
            std::lock_guard<std::mutex> lock(g_ringsMutex);
            rings = g_rings;
        }

        Calib calib = MakeCalib();
        ThreadEvents out;
        for (Ring* ring : rings)
        {
            uint64_t head = ring->head.load(std::memory_order_acquire);
            uint64_t first = head > kRingSize ? head - kRingSize : 0;

            for (uint64_t ticket = first; ticket < head; ++ticket)
            {
                Event e;
                if (!ring->Read(ticket, e))
                {
                    continue;
                }
                if (IsClosing(e.type))
                {
                    CloseOpenEvent(out, e);
                }
                else if (IsOpening(e.type))
                {
                    e.timestampNs = calib.ToNs(e.timestampNs);
                    out[e.osThreadId].push_back(e);
                }
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
