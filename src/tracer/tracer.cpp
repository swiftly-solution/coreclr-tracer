#include "tracer.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <sys/syscall.h>
#include <unistd.h>
#endif

static constexpr size_t kRingSize = 1 << 16;
static constexpr size_t kRingMask = kRingSize - 1;

static uint32_t CurrentOsThreadId() {
#ifdef _WIN32
  return GetCurrentThreadId();
#else
  return static_cast<uint32_t>(syscall(SYS_gettid));
#endif
}

static const auto g_start = std::chrono::steady_clock::now();

static uint64_t NowNs() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now() - g_start)
          .count());
}

struct Slot {
  std::atomic<uint64_t> seq{0};
  Event ev{};
};

class Tracer final : public ITracer {
  Slot m_ring[kRingSize];
  std::atomic<uint64_t> m_head{0};

  std::atomic<bool> m_profiling{false};
  std::mutex m_profileMutex;
  std::vector<Event> m_profile;

  std::mutex m_stringMutex;
  std::deque<std::string> m_strings;
  std::unordered_map<std::string, uint32_t> m_stringIds;

public:
  uint32_t Intern(const std::string &s) {
    if (s.empty()) {
      return 0;
    }
    std::lock_guard<std::mutex> lock(m_stringMutex);
    auto it = m_stringIds.find(s);
    if (it != m_stringIds.end()) {
      return it->second;
    }
    m_strings.push_back(s);
    uint32_t id = static_cast<uint32_t>(m_strings.size());
    m_stringIds.emplace(s, id);
    return id;
  }

  const char *GetString(uint32_t id) override {
    std::lock_guard<std::mutex> lock(m_stringMutex);
    return id != 0 && id <= m_strings.size() ? m_strings[id - 1].c_str() : "";
  }

  void Record(const Event &e) {
    uint64_t ticket = m_head.fetch_add(1, std::memory_order_acq_rel);
    Slot &s = m_ring[ticket & kRingMask];
    s.seq.store(2 * ticket + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    s.ev = e;
    s.seq.store(2 * ticket + 2, std::memory_order_release);

    if (m_profiling.load(std::memory_order_acquire)) {
      std::lock_guard<std::mutex> lock(m_profileMutex);
      if (m_profiling.load(std::memory_order_relaxed)) {
        m_profile.push_back(e);
      }
    }
  }

  std::vector<Event> TakeCrashSnapshot() override {
    std::vector<Event> out;
    uint64_t head = m_head.load(std::memory_order_acquire);
    uint64_t first = head > kRingSize ? head - kRingSize : 0;
    out.reserve(head - first);

    for (uint64_t t = first; t < head; ++t) {
      Slot &s = m_ring[t & kRingMask];
      uint64_t before = s.seq.load(std::memory_order_acquire);
      if (before != 2 * t + 2) {
        continue;
      }
      Event copy = s.ev;
      std::atomic_thread_fence(std::memory_order_acquire);
      if (s.seq.load(std::memory_order_relaxed) != before) {
        continue;
      }
      out.push_back(copy);
    }
    return out;
  }

  void StartProfiling() override {
    std::lock_guard<std::mutex> lock(m_profileMutex);
    m_profiling.store(true, std::memory_order_release);
  }

  std::vector<Event> TakeProfileSnapshot() override {
    std::lock_guard<std::mutex> lock(m_profileMutex);
    return m_profile;
  }

  void StopProfiling() override {
    std::lock_guard<std::mutex> lock(m_profileMutex);
    m_profiling.store(false, std::memory_order_release);
    std::vector<Event>().swap(m_profile);
  }

  void CustomBegin(const char *name, const char *source) override {
    Record(Event{NowNs(), EventType::CustomBegin, CurrentOsThreadId(), 0, 0,
                 Intern(name != nullptr ? name : ""),
                 Intern(source != nullptr ? source : "")});
  }

  void CustomEnd(const char *name) override {
    Record(Event{NowNs(), EventType::CustomEnd, CurrentOsThreadId(), 0, 0,
                 Intern(name != nullptr ? name : ""), 0});
  }
};

static Tracer g_tracer;

void RecordEvent(EventType type, uint64_t a, uint64_t b, uint32_t nameId,
                 uint32_t sourceId) {
  g_tracer.Record(
      Event{NowNs(), type, CurrentOsThreadId(), a, b, nameId, sourceId});
}

uint32_t InternString(const std::string &s) { return g_tracer.Intern(s); }

#ifdef _WIN32
#define TRACER_EXPORT extern "C"
#else
#define TRACER_EXPORT extern "C" __attribute__((visibility("default")))
#endif

TRACER_EXPORT ITracer *GetTracer() { return &g_tracer; }

// C# cannot consume std::vector, so the test harness copies into its own buffer.
TRACER_EXPORT size_t TracerCrashSnapshot(Event *out, size_t capacity) {
  std::vector<Event> events = g_tracer.TakeCrashSnapshot();
  size_t n = std::min(capacity, events.size());
  std::copy_n(events.begin(), n, out);
  return n;
}
