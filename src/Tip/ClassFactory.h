#pragma once
#include <unknwn.h>

namespace llaime {

class ClassFactory final : public IClassFactory {
 public:
  STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
  STDMETHODIMP_(ULONG) AddRef() override;
  STDMETHODIMP_(ULONG) Release() override;
  STDMETHODIMP CreateInstance(IUnknown* outer, REFIID riid, void** ppv) override;
  STDMETHODIMP LockServer(BOOL lock) override;
};

}  // namespace llaime
