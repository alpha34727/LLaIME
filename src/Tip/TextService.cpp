#include "TextService.h"

#include "Globals.h"

using Microsoft::WRL::ComPtr;

namespace llaime {

TextService::TextService() { DllAddRef(); }

TextService::~TextService() { DllRelease(); }

STDMETHODIMP TextService::QueryInterface(REFIID riid, void** ppv) {
  if (!ppv) return E_INVALIDARG;
  *ppv = nullptr;
  if (riid == IID_IUnknown || riid == IID_ITfTextInputProcessor ||
      riid == IID_ITfTextInputProcessorEx) {
    *ppv = static_cast<ITfTextInputProcessorEx*>(this);
  } else if (riid == IID_ITfThreadMgrEventSink) {
    *ppv = static_cast<ITfThreadMgrEventSink*>(this);
  } else if (riid == IID_ITfKeyEventSink) {
    *ppv = static_cast<ITfKeyEventSink*>(this);
  } else {
    return E_NOINTERFACE;
  }
  AddRef();
  return S_OK;
}

STDMETHODIMP_(ULONG) TextService::AddRef() { return InterlockedIncrement(&refCount_); }

STDMETHODIMP_(ULONG) TextService::Release() {
  LONG count = InterlockedDecrement(&refCount_);
  if (count == 0) delete this;
  return count;
}

STDMETHODIMP TextService::Activate(ITfThreadMgr* threadMgr, TfClientId clientId) {
  return ActivateEx(threadMgr, clientId, 0);
}

STDMETHODIMP TextService::ActivateEx(ITfThreadMgr* threadMgr, TfClientId clientId, DWORD) {
  threadMgr_ = threadMgr;
  clientId_ = clientId;
  if (!InitThreadMgrEventSink() || !InitKeyEventSink()) {
    Deactivate();
    return E_FAIL;
  }
  return S_OK;
}

STDMETHODIMP TextService::Deactivate() {
  UninitKeyEventSink();
  UninitThreadMgrEventSink();
  threadMgr_.Reset();
  clientId_ = TF_CLIENTID_NULL;
  return S_OK;
}

bool TextService::InitThreadMgrEventSink() {
  ComPtr<ITfSource> source;
  if (FAILED(threadMgr_.As(&source))) return false;
  return SUCCEEDED(source->AdviseSink(IID_ITfThreadMgrEventSink,
                                      static_cast<ITfThreadMgrEventSink*>(this),
                                      &threadMgrEventSinkCookie_));
}

void TextService::UninitThreadMgrEventSink() {
  if (threadMgrEventSinkCookie_ == TF_INVALID_COOKIE || !threadMgr_) return;
  ComPtr<ITfSource> source;
  if (SUCCEEDED(threadMgr_.As(&source))) source->UnadviseSink(threadMgrEventSinkCookie_);
  threadMgrEventSinkCookie_ = TF_INVALID_COOKIE;
}

STDMETHODIMP TextService::OnInitDocumentMgr(ITfDocumentMgr*) { return S_OK; }
STDMETHODIMP TextService::OnUninitDocumentMgr(ITfDocumentMgr*) { return S_OK; }
STDMETHODIMP TextService::OnSetFocus(ITfDocumentMgr*, ITfDocumentMgr*) { return S_OK; }
STDMETHODIMP TextService::OnPushContext(ITfContext*) { return S_OK; }
STDMETHODIMP TextService::OnPopContext(ITfContext*) { return S_OK; }

}  // namespace llaime
