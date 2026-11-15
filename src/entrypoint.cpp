#include "rpc.h"
#include "guiddef.h"

const IID CLSID_Profiler = {0x2e7d6025, 0x9ef7, 0x4942, {0xb3, 0x5e, 0x4a, 0x9d, 0x3c, 0x4a, 0xa0, 0x24}};

BOOL STDMETHODCALLTYPE DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved)
{
    return TRUE;
}

STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, LPVOID *ppv)
{
    return E_FAIL;
}

STDAPI DllCanUnloadNow()
{
    return S_OK;
}
