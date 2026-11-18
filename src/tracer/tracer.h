#ifndef _src_tracer_tracer_h
#define _src_tracer_tracer_h

#include "../shared/api.h"
#include <string_view>

void RecordEvent(EventType type, uint64_t a = 0, uint64_t b = 0, uint32_t nameId = 0, uint32_t sourceId = 0);

uint32_t InternString(std::string_view s);

bool ProfilerSetObjectTracking(bool enabled);
bool ProfilerObjectTracking();
uint32_t ProfilerThreadNameId(uint32_t osThreadId);

#endif
