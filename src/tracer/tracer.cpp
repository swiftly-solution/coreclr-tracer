#include "tracer.h"
#include "clock.h"
#include "profile_buffer.h"
#include "profile_writer.h"
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
    std::atomic<ProfileBuffer*> profile{nullptr};
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

static std::vector<ProfileBuffer*> ProfileBuffers()
{
    std::vector<ProfileBuffer*> out;
    std::lock_guard<std::mutex> lock(g_ringsMutex);
    for (Ring* ring : g_rings)
    {
        if (ProfileBuffer* buf = ring->profile.load(std::memory_order_acquire))
        {
            out.push_back(buf);
        }
    }
    return out;
}

constexpr uint64_t kGoldenRatio64 = 0x9E3779B97F4A7C15ull;

static size_t HashCombine(size_t seed, uint64_t value)
{
    return seed ^ (std::hash<uint64_t>{}(value) + kGoldenRatio64 + (seed << 6) + (seed >> 2));
}

struct FnKeyHash
{
    size_t operator()(const FnKey& key) const
    {
        size_t hash = std::hash<uint64_t>{}(key.a);
        hash = HashCombine(hash, key.b);
        hash = HashCombine(hash, key.nameId);
        hash = HashCombine(hash, key.sourceId);
        return hash;
    }
};

class FnTable
{
public:
    // Returns 0 when the table is full.
    uint32_t Index(const FnKey& key)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_ids.find(key);
        if (it != m_ids.end())
        {
            return it->second;
        }

        uint32_t id = m_count.load(std::memory_order_relaxed) + 1;
        if (id > kChunk * kChunks)
        {
            return 0;
        }

        FnKey* chunk = m_chunks[(id - 1) / kChunk].load(std::memory_order_relaxed);
        if (chunk == nullptr)
        {
            chunk = new FnKey[kChunk];
            m_chunks[(id - 1) / kChunk].store(chunk, std::memory_order_relaxed);
        }
        chunk[(id - 1) % kChunk] = key;
        m_ids.emplace(key, id);
        m_count.store(id, std::memory_order_release);
        return id;
    }

    FnKey Get(uint32_t id) const
    {
        if (id == 0 || id > m_count.load(std::memory_order_acquire))
        {
            return FnKey{};
        }
        return m_chunks[(id - 1) / kChunk].load(std::memory_order_relaxed)[(id - 1) % kChunk];
    }

private:
    static constexpr uint32_t kChunk = 4096;
    static constexpr uint32_t kChunks = 1024;

    std::mutex m_mutex;
    std::atomic<uint32_t> m_count{0};
    std::atomic<FnKey*> m_chunks[kChunks]{};
    std::unordered_map<FnKey, uint32_t, FnKeyHash> m_ids;
};

struct FnCacheEntry
{
    FnKey key;
    uint32_t index;
};

constexpr unsigned kFnCacheBits = 8;
constexpr size_t kFnCacheSize = size_t{1} << kFnCacheBits;

static thread_local FnCacheEntry tl_fnCache[kFnCacheSize];

static size_t FnCacheSlot(const FnKey& key)
{
    return static_cast<size_t>((key.a * kGoldenRatio64) >> (64 - kFnCacheBits));
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
        Ring* ring = CurrentRing();
        ring->Push(e);

        if (m_profiling.load(std::memory_order_acquire))
        {
            RecordProfile(ring, e);
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
        m_profiling.store(true, std::memory_order_release);
    }

    bool WriteProfile(ProfileBlockWriter write, void* context) override
    {
        if (write == nullptr)
        {
            return false;
        }

        std::shared_lock<std::shared_mutex> saving(m_saveMutex);

        std::vector<ProfileBuffer::View> views;
        for (ProfileBuffer* buf : ProfileBuffers())
        {
            std::lock_guard<std::mutex> lock(buf->mutex);
            ProfileBuffer::View view = buf->Freeze();
            if (!view.Empty())
            {
                views.push_back(view);
            }
        }

        Calib calib = MakeCalib();
        uint64_t endOfTraceTicks = 0;
        for (const ProfileBuffer::View& view : views)
        {
            endOfTraceTicks = (std::max)(endOfTraceTicks, view.LastTimestamp());
        }

        ProfileWriter writer(write, context);
        auto resolve = [this](uint32_t index) { return m_fns.Get(index); };
        for (const ProfileBuffer::View& view : views)
        {
            view.ForEach(resolve, [&](const Event& e) {
                writer.Add(e, calib.ToNs(e.timestampNs));
            });
            if (writer.Failed())
            {
                return false;
            }
        }

        uint32_t stringCount = m_stringCount.load(std::memory_order_acquire);
        return writer.Finish(calib.ToNs(endOfTraceTicks), stringCount, ProfilerThreadNameId, [this](uint32_t id) { return std::string_view(GetString(id)); });
    }

    void StopProfiling() override
    {
        m_profiling.store(false, std::memory_order_release);
        std::unique_lock<std::shared_mutex> saving(m_saveMutex);
        for (ProfileBuffer* buf : ProfileBuffers())
        {
            std::lock_guard<std::mutex> lock(buf->mutex);
            buf->Clear();
        }
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
    uint32_t FunctionIndex(const Event& e)
    {
        FnKey key{e.a, e.b, e.nameId, e.sourceId};
        FnCacheEntry& cached = tl_fnCache[FnCacheSlot(key)];
        if (cached.index != 0 && cached.key == key)
        {
            return cached.index;
        }

        uint32_t index = m_fns.Index(key);
        if (index != 0)
        {
            cached = FnCacheEntry{key, index};
        }
        return index;
    }

    void RecordProfile(Ring* ring, const Event& e)
    {
        ProfileBuffer* buf = ring->profile.load(std::memory_order_relaxed);
        if (buf == nullptr)
        {
            buf = new ProfileBuffer();
            ring->profile.store(buf, std::memory_order_release);
        }

        uint32_t fn = 0;
        if (IsFunctionEvent(e.type))
        {
            fn = FunctionIndex(e);
            if (fn == 0)
            {
                return;
            }
        }

        std::lock_guard<std::mutex> lock(buf->mutex);
        if (!m_profiling.load(std::memory_order_relaxed))
        {
            return;
        }
        if (fn != 0)
        {
            buf->AppendFunction(e.timestampNs, e.type, e.osThreadId, fn);
        }
        else
        {
            buf->AppendEvent(e);
        }
    }

    std::atomic<bool> m_profiling{false};
    std::shared_mutex m_saveMutex;
    FnTable m_fns;

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
