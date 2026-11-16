#ifndef _src_coreclr_classfactory_h
#define _src_coreclr_classfactory_h

#include "unknwn.h"
#include <atomic>
#include <unknwnbase.h>
#include <winnt.h>

class ClassFactory : public IClassFactory
{
private:
    std::atomic<int> refCount;
public:
    ClassFactory();
    virtual ~ClassFactory();
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppObject) override;
    ULONG STDMETHODCALLTYPE AddRef(void) override;
    ULONG STDMETHODCALLTYPE Release(void) override;
    HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* pOuter, REFIID riid, void** ppObject) override;
    HRESULT STDMETHODCALLTYPE LockServer(BOOL fLock) override;
};

#endif
