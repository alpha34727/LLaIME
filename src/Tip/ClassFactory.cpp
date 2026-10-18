#include "ClassFactory.h"

#include <new>

#include "Globals.h"
#include "Guard.h"
#include "TextService.h"

namespace llaime {

STDMETHODIMP ClassFactory::QueryInterface(REFIID riid, void** ppv) {
  if (!ppv) return E_INVALIDARG;
  if (riid == IID_IUnknown || riid == IID_IClassFactory) {
    *ppv = static_cast<IClassFactory*>(this);
    AddRef();
    return S_OK;
  }
  *ppv = nullptr;
  return E_NOINTERFACE;
}

// The factory is a static singleton; its lifetime is tied to the DLL ref count.
STDMETHODIMP_(ULONG) ClassFactory::AddRef() {
  DllAddRef();
  return 2;
}

STDMETHODIMP_(ULONG) ClassFactory::Release() {
  DllRelease();
  return 1;
}

STDMETHODIMP ClassFactory::CreateInstance(IUnknown* outer, REFIID riid, void** ppv) {
  if (!ppv) return E_INVALIDARG;
  *ppv = nullptr;
  if (outer) return CLASS_E_NOAGGREGATION;
  return Guarded("CreateInstance", HRESULT(E_FAIL), [&]() -> HRESULT {
    auto* service = new (std::nothrow) TextService();
    if (!service) return E_OUTOFMEMORY;
    HRESULT hr = service->QueryInterface(riid, ppv);
    service->Release();
    return hr;
  });
}

STDMETHODIMP ClassFactory::LockServer(BOOL lock) {
  if (lock) {
    DllAddRef();
  } else {
    DllRelease();
  }
  return S_OK;
}

}  // namespace llaime
