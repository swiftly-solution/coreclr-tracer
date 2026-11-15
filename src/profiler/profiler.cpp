#include "../shared/extern.h"
#ifdef _WIN32
#include <windows.h>
#define IMAGE_DATA_DIRECTORY_DEFINED // SDK winnt.h already defines this struct
#endif
#include "cor.h"
#include "corprof.h"

PROFILER_STUB EnterStub(FunctionIDOrClientID functionId, COR_PRF_ELT_INFO eltInfo)
{

}

PROFILER_STUB ExitStub(FunctionIDOrClientID functionId, COR_PRF_ELT_INFO eltInfo)
{
}

PROFILER_STUB TailcallStub(FunctionIDOrClientID functionId, COR_PRF_ELT_INFO eltInfo)
{
}
