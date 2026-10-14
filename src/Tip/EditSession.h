#pragma once
#include <msctf.h>

#include <functional>
#include <new>
#include <utility>

namespace llaime {

// ITfEditSession that runs a callable with the granted edit cookie.
class EditSession final : public ITfEditSession {
 public:
  using Fn = std::function<HRESULT(TfEditCookie)>;

  // Requests a session on `context`. Synchronous sessions run before this returns.
  static HRESULT Request(ITfContext* context, TfClientId clientId, DWORD flags, Fn fn) {
    auto* session = new (std::nothrow) EditSession(std::move(fn));
    if (!session) return E_OUTOFMEMORY;
    HRESULT hrSession = E_FAIL;
    HRESULT hr = context->RequestEditSession(clientId, session, flags, &hrSession);
    session->Release();
    return FAILED(hr) ? hr : hrSession;
  }

  STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
    if (!ppv) return E_INVALIDARG;
    if (riid == IID_IUnknown || riid == IID_ITfEditSession) {
      *ppv = static_cast<ITfEditSession*>(this);
      AddRef();
      return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
  }
  STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&ref_); }
  STDMETHODIMP_(ULONG) Release() override {
    LONG r = InterlockedDecrement(&ref_);
    if (r == 0) delete this;
    return r;
  }
  STDMETHODIMP DoEditSession(TfEditCookie ec) override { return fn_(ec); }

 private:
  explicit EditSession(Fn fn) : fn_(std::move(fn)) {}
  ~EditSession() = default;

  LONG ref_ = 1;
  Fn fn_;
};

}  // namespace llaime
