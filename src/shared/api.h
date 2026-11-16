#ifndef _src_shared_api_h
#define _src_shared_api_h

#include <cstddef>
#include <cstdint>
#include <vector>

enum class EventType : uint32_t {
  FunctionEnter,    // a = FunctionID, b = code address, nameId = function name,
                    // sourceId = module path (0 if unknown)
  FunctionExit,     // same as FunctionEnter
  FunctionTailcall, // same as FunctionEnter
  ThreadCreated,    // a = managed ThreadID, b = OS thread id (0 until assigned),
                    // nameId = thread name (usually 0: name is set later)
  ThreadDestroyed,  // a = managed ThreadID, b = OS thread id, nameId = last name
  ThreadAssignedToOSThread, // a = managed ThreadID, b = OS thread id, nameId
  UnmanagedToManaged, // a = FunctionID, b = COR_PRF_TRANSITION_REASON,
                      // nameId = function name
  ObjectAllocated,    // a = ObjectID, b = size in bytes, nameId = object type
                      // name, sourceId = allocating function name (0 if unknown)
  ThreadNameChanged,  // a = managed ThreadID, b = OS thread id, nameId = new name
  CustomBegin,        // nameId = section name, sourceId = user source (0 if none).
                      // Start time = timestampNs.
  CustomEnd,          // nameId = section name. Stop time = timestampNs. Pairs with
                      // the latest open CustomBegin of the same name on the same
                      // osThreadId.
};

struct Event {
  uint64_t timestampNs;
  EventType type;
  uint32_t osThreadId;
  uint64_t a;
  uint64_t b;
  uint32_t nameId;
  uint32_t sourceId;
};
static_assert(sizeof(Event) == 40, "Event layout is part of the API");

class ITracer {
public:
  virtual const char *GetString(uint32_t id) = 0;

  virtual std::vector<Event> TakeCrashSnapshot() = 0;

  virtual void StartProfiling() = 0;
  virtual std::vector<Event> TakeProfileSnapshot() = 0;
  virtual void StopProfiling() = 0;

  virtual void CustomBegin(const char *name, const char *source) = 0;
  virtual void CustomEnd(const char *name) = 0;

protected:
  ~ITracer() = default;
};

using GetTracerFn = ITracer *(*)();

#endif
