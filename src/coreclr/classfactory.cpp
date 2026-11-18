#include "classfactory.h"
#include "../profiler/profiler.h"

#ifndef _WIN32
#include "Guid.h"
DEFINE_GUID(IID_IUnknown, 0x00000000, 0, 0, 0xC0, 0, 0, 0, 0, 0, 0, 0x46);
DEFINE_GUID(IID_IClassFactory, 0x00000001, 0, 0, 0xC0, 0, 0, 0, 0, 0, 0, 0x46);
DEFINE_GUID(IID_ICorProfilerCallback, 0x176FBED1, 0xA55C, 0x4796, 0x98, 0xCA, 0xA9, 0xDA, 0x0E, 0xF8, 0x83, 0xE7);
DEFINE_GUID(IID_ICorProfilerCallback2, 0x8A8CC829, 0xCCF2, 0x49FE, 0xBB, 0xAE, 0x0F, 0x02, 0x22, 0x28, 0x07, 0x1A);
DEFINE_GUID(IID_ICorProfilerCallback3, 0x4FD2ED52, 0x7731, 0x4B8D, 0x94, 0x69, 0x03, 0xD2, 0xCC, 0x30, 0x86, 0xC5);
DEFINE_GUID(IID_ICorProfilerCallback4, 0x7B63B2E3, 0x107D, 0x4D48, 0xB2, 0xF6, 0xF6, 0x1E, 0x22, 0x94, 0x70, 0xD2);
DEFINE_GUID(IID_ICorProfilerCallback5, 0x8DFBA405, 0x8C9F, 0x45F8, 0xBF, 0xFA, 0x83, 0xB1, 0x4C, 0xEF, 0x78, 0xB5);
DEFINE_GUID(IID_ICorProfilerCallback6, 0xFC13DF4B, 0x4448, 0x4F4F, 0x95, 0x0C, 0xBA, 0x8D, 0x19, 0xD0, 0x0C, 0x36);
DEFINE_GUID(IID_ICorProfilerCallback7, 0xF76A2DBA, 0x1D52, 0x4539, 0x86, 0x6C, 0x2A, 0xA5, 0x18, 0xF9, 0xEF, 0xC3);
DEFINE_GUID(IID_ICorProfilerCallback8, 0x5BED9B15, 0xC079, 0x4D47, 0xBF, 0xE2, 0x21, 0x5A, 0x14, 0x0C, 0x07, 0xE0);
DEFINE_GUID(IID_ICorProfilerCallback9, 0x27583EC3, 0xC8F5, 0x482F, 0x80, 0x52, 0x19, 0x4B, 0x8C, 0xE4, 0x70, 0x5A);
DEFINE_GUID(IID_ICorProfilerCallback10, 0xCEC5B60E, 0xC69C, 0x495F, 0x87, 0xF6, 0x84, 0xD2, 0x8E, 0xE1, 0x6F, 0xFB);
DEFINE_GUID(IID_ICorProfilerInfo10, 0x2F1B5152, 0xC869, 0x40C9, 0xAA, 0x5F, 0x3A, 0xBE, 0x02, 0x6B, 0xD7, 0x20);
#endif

ClassFactory::ClassFactory() : refCount(0) {}

ClassFactory::~ClassFactory() {}

HRESULT STDMETHODCALLTYPE ClassFactory::QueryInterface(REFIID riid, void** ppObject)
{
    if (riid == IID_IUnknown || riid == IID_IClassFactory)
    {
        *ppObject = this;
        this->AddRef();
        return S_OK;
    }

    *ppObject = nullptr;
    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE ClassFactory::AddRef()
{
    return std::atomic_fetch_add(&this->refCount, 1) + 1;
}

ULONG STDMETHODCALLTYPE ClassFactory::Release()
{
    int count = std::atomic_fetch_sub(&this->refCount, 1) - 1;
    if (count <= 0)
    {
        delete this;
    }

    return count;
}

HRESULT STDMETHODCALLTYPE ClassFactory::CreateInstance(IUnknown* pUnkOuter, REFIID riid, void** ppObject)
{
    if (pUnkOuter != nullptr)
    {
        *ppObject = nullptr;
        return CLASS_E_NOAGGREGATION;
    }

    CorProfiler* profiler = new CorProfiler();
    if (profiler == nullptr)
    {
        return E_FAIL;
    }

    return profiler->QueryInterface(riid, ppObject);
}

HRESULT STDMETHODCALLTYPE ClassFactory::LockServer(BOOL fLock)
{
    return S_OK;
}
