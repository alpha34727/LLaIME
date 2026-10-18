// The candidate list as a TSF UI element. Apps that draw the candidates themselves (Windows
// Search, Sticky Notes: their windows are not in a layer a TIP's own window can show in) say
// so when the list is announced (BeginUIElement returns show = FALSE), and then read it from
// here; the key handling and the list's state stay in CandidateWindow either way.
#include "Globals.h"
#include "Guard.h"
#include "TextService.h"
#include "ui/CandidateWindow.h"

using Microsoft::WRL::ComPtr;

namespace llaime {
namespace {

// {2f1c5d6e-8b0a-4c8e-9a52-3d7a6b1e4f90}
constexpr GUID kCandidateListGuid = {
    0x2f1c5d6e, 0x8b0a, 0x4c8e, {0x9a, 0x52, 0x3d, 0x7a, 0x6b, 0x1e, 0x4f, 0x90}};

constexpr UINT kPageSize = CandidateWindow::kRows;

}  // namespace

void TextService::BeginCandidateUi() {
  ComPtr<ITfUIElementMgr> mgr;
  if (!threadMgr_ || FAILED(threadMgr_.As(&mgr))) return;
  BOOL show = TRUE;
  DWORD id = 0;
  if (FAILED(mgr->BeginUIElement(static_cast<ITfCandidateListUIElement*>(this), &show, &id))) {
    return;
  }
  candidateUiId_ = id;
  candidateUiActive_ = true;
  candidateWindow_->SetUiLess(!show);
}

void TextService::UpdateCandidateUi() {
  if (!candidateUiActive_ || !threadMgr_) return;
  ComPtr<ITfUIElementMgr> mgr;
  if (SUCCEEDED(threadMgr_.As(&mgr))) mgr->UpdateUIElement(candidateUiId_);
}

void TextService::EndCandidateUi() {
  if (!candidateUiActive_) return;
  candidateUiActive_ = false;
  if (candidateWindow_) candidateWindow_->SetUiLess(false);
  ComPtr<ITfUIElementMgr> mgr;
  if (threadMgr_ && SUCCEEDED(threadMgr_.As(&mgr))) mgr->EndUIElement(candidateUiId_);
}

STDMETHODIMP TextService::GetDescription(BSTR* description) {
  if (!description) return E_INVALIDARG;
  *description = SysAllocString(L"LLaIME 候選字");
  return *description ? S_OK : E_OUTOFMEMORY;
}

STDMETHODIMP TextService::GetGUID(GUID* guid) {
  if (!guid) return E_INVALIDARG;
  *guid = kCandidateListGuid;
  return S_OK;
}

// The app asking us to show or hide: ours is shown by CandidateWindow, or by the app.
STDMETHODIMP TextService::Show(BOOL) { return S_OK; }

STDMETHODIMP TextService::IsShown(BOOL* shown) {
  if (!shown) return E_INVALIDARG;
  *shown = candidateWindow_ && candidateWindow_->IsVisible() && !candidateWindow_->UiLess();
  return S_OK;
}

STDMETHODIMP TextService::GetUpdatedFlags(DWORD* flags) {
  if (!flags) return E_INVALIDARG;
  *flags = TF_CLUIE_DOCUMENTMGR | TF_CLUIE_COUNT | TF_CLUIE_SELECTION | TF_CLUIE_STRING |
           TF_CLUIE_PAGEINDEX | TF_CLUIE_CURRENTPAGE;
  return S_OK;
}

STDMETHODIMP TextService::GetDocumentMgr(ITfDocumentMgr** docMgr) {
  if (!docMgr) return E_INVALIDARG;
  *docMgr = nullptr;
  return threadMgr_ ? threadMgr_->GetFocus(docMgr) : E_FAIL;
}

STDMETHODIMP TextService::GetCount(UINT* count) {
  if (!count) return E_INVALIDARG;
  *count = candidateWindow_ ? static_cast<UINT>(candidateWindow_->Count()) : 0;
  return S_OK;
}

STDMETHODIMP TextService::GetSelection(UINT* index) {
  if (!index) return E_INVALIDARG;
  *index = candidateWindow_ ? static_cast<UINT>(candidateWindow_->Selection()) : 0;
  return S_OK;
}

STDMETHODIMP TextService::GetString(UINT index, BSTR* text) {
  if (!text) return E_INVALIDARG;
  *text = nullptr;
  if (!candidateWindow_ || index >= candidateWindow_->Count()) return E_INVALIDARG;
  *text = SysAllocString(candidateWindow_->Item(index).c_str());
  return *text ? S_OK : E_OUTOFMEMORY;
}

STDMETHODIMP TextService::GetPageIndex(UINT* index, UINT size, UINT* pageCount) {
  if (!pageCount) return E_INVALIDARG;
  const UINT count = candidateWindow_ ? static_cast<UINT>(candidateWindow_->Count()) : 0;
  const UINT pages = (count + kPageSize - 1) / kPageSize;
  *pageCount = pages;
  if (index) {
    for (UINT i = 0; i < pages && i < size; ++i) index[i] = i * kPageSize;
  }
  return S_OK;
}

STDMETHODIMP TextService::SetPageIndex(UINT*, UINT) { return S_OK; }

STDMETHODIMP TextService::GetCurrentPage(UINT* page) {
  if (!page) return E_INVALIDARG;
  *page = candidateWindow_ ? static_cast<UINT>(candidateWindow_->Selection() / kPageSize) : 0;
  return S_OK;
}

}  // namespace llaime
