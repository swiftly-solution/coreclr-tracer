#ifndef _src_tracer_tracer_h
#define _src_tracer_tracer_h

#include "../shared/api.h"
#include <string>

void RecordEvent(EventType type, uint64_t a = 0, uint64_t b = 0,
                 uint32_t nameId = 0, uint32_t sourceId = 0);

uint32_t InternString(const std::string &s);

#endif
