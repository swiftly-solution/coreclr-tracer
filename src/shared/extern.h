#ifndef _src_shared_extern_h
#define _src_shared_extern_h

#include <atomic>
#include <cstdlib>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#ifndef _WIN32
#include "pal_mstypes.h"
#include "pal.h"
#include "ntimage.h"
#include "corhdr.h"

#define PROFILER_STUB EXTERN_C __attribute__((visibility("hidden"))) void STDMETHODCALLTYPE
#else
#define PROFILER_STUB EXTERN_C void STDMETHODCALLTYPE
#endif

#endif
