#include "TextService.h"

using Microsoft::WRL::ComPtr;

namespace llaime {

bool TextService::InitKeyEventSink() {
  ComPtr<ITfKeystrokeMgr> keystrokeMgr;
  if (FAILED(threadMgr_.As(&keystrokeMgr))) return false;
  return SUCCEEDED(
      keystrokeMgr->AdviseKeyEventSink(clientId_, static_cast<ITfKeyEventSink*>(this), TRUE));
}

void TextService::UninitKeyEventSink() {
  if (!threadMgr_) return;
  ComPtr<ITfKeystrokeMgr> keystrokeMgr;
  if (SUCCEEDED(threadMgr_.As(&keystrokeMgr))) keystrokeMgr->UnadviseKeyEventSink(clientId_);
}

STDMETHODIMP TextService::OnSetFocus(BOOL) { return S_OK; }

// Stage 1: every key passes through to the application unchanged.
STDMETHODIMP TextService::OnTestKeyDown(ITfContext*, WPARAM, LPARAM, BOOL* eaten) {
  *eaten = FALSE;
  return S_OK;
}

STDMETHODIMP TextService::OnKeyDown(ITfContext*, WPARAM, LPARAM, BOOL* eaten) {
  *eaten = FALSE;
  return S_OK;
}

STDMETHODIMP TextService::OnTestKeyUp(ITfContext*, WPARAM, LPARAM, BOOL* eaten) {
  *eaten = FALSE;
  return S_OK;
}

STDMETHODIMP TextService::OnKeyUp(ITfContext*, WPARAM, LPARAM, BOOL* eaten) {
  *eaten = FALSE;
  return S_OK;
}

STDMETHODIMP TextService::OnPreservedKey(ITfContext*, REFGUID, BOOL* eaten) {
  *eaten = FALSE;
  return S_OK;
}

}  // namespace llaime
