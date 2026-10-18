#include <algorithm>

#include "EditSession.h"
#include "Globals.h"
#include "Guard.h"
#include "TextService.h"
#include "ui/CandidateWindow.h"

using Microsoft::WRL::ComPtr;

namespace llaime {
namespace {

// Once the buffer grows past kMaxElements, the leading phrases are committed so the
// composition stays short, keeping at least kKeepElements for context-aware conversion.
constexpr size_t kMaxElements = 40;
constexpr size_t kKeepElements = 20;

ComPtr<ITfRange> SubRange(ITfRange* whole, TfEditCookie ec, LONG begin, LONG end) {
  ComPtr<ITfRange> r;
  if (FAILED(whole->Clone(&r))) return nullptr;
  LONG moved = 0;
  r->Collapse(ec, TF_ANCHOR_START);
  r->ShiftEnd(ec, end, &moved, nullptr);
  r->ShiftStart(ec, begin, &moved, nullptr);
  return r;
}

void SetCaret(ITfContext* context, TfEditCookie ec, ITfRange* whole, LONG offset) {
  ComPtr<ITfRange> caret = SubRange(whole, ec, offset, offset);
  if (!caret) return;
  TF_SELECTION sel{};
  sel.range = caret.Get();
  sel.style.ase = TF_AE_NONE;
  sel.style.fInterimChar = FALSE;
  context->SetSelection(ec, 1, &sel);
}

void ClearDisplayAttribute(ITfContext* context, TfEditCookie ec, ITfRange* range) {
  ComPtr<ITfProperty> prop;
  if (SUCCEEDED(context->GetProperty(GUID_PROP_ATTRIBUTE, &prop))) prop->Clear(ec, range);
}

// Rect from GetTextExt is in the host window's coordinate space; convert to physical pixels
// for DPI-unaware or system-aware hosts, since the candidate window is per-monitor aware.
void ToPhysical(HWND hwnd, RECT* rc) {
  if (!hwnd) return;
  DPI_AWARENESS awareness =
      GetAwarenessFromDpiAwarenessContext(GetWindowDpiAwarenessContext(hwnd));
  if (awareness == DPI_AWARENESS_PER_MONITOR_AWARE) return;
  POINT tl{rc->left, rc->top}, br{rc->right, rc->bottom};
  LogicalToPhysicalPointForPerMonitorDPI(hwnd, &tl);
  LogicalToPhysicalPointForPerMonitorDPI(hwnd, &br);
  *rc = {tl.x, tl.y, br.x, br.y};
}

}  // namespace

bool TextService::StartComposition(ITfContext* context, TfEditCookie ec) {
  ComPtr<ITfInsertAtSelection> insert;
  ComPtr<ITfContextComposition> contextComposition;
  ComPtr<ITfRange> range;
  if (FAILED(context->QueryInterface(IID_PPV_ARGS(&insert))) ||
      FAILED(context->QueryInterface(IID_PPV_ARGS(&contextComposition))) ||
      FAILED(insert->InsertTextAtSelection(ec, TF_IAS_QUERYONLY, nullptr, 0, &range))) {
    return false;
  }
  ComPtr<ITfComposition> composition;
  if (FAILED(contextComposition->StartComposition(
          ec, range.Get(), static_cast<ITfCompositionSink*>(this), &composition)) ||
      !composition) {
    return false;
  }
  composition_ = composition;
  compositionContext_ = context;
  // Re-check privacy at the start of every composition: focus events alone miss changes
  // inside one document (e.g. moving between a search box and a password box in a page).
  UpdatePrivacy(context, ec);
  ReadContextBefore(context, ec, range.Get());
  return true;
}

void TextService::EndComposition(TfEditCookie ec) {
  if (composition_) composition_->EndComposition(ec);
  composition_.Reset();
  compositionContext_.Reset();
}

void TextService::ApplyDisplayAttribute(ITfContext* context, TfEditCookie ec, ITfRange* range,
                                        TfGuidAtom atom) {
  if (atom == TF_INVALID_GUIDATOM) return;
  ComPtr<ITfProperty> prop;
  if (FAILED(context->GetProperty(GUID_PROP_ATTRIBUTE, &prop))) return;
  VARIANT var;
  VariantInit(&var);
  var.vt = VT_I4;
  var.lVal = static_cast<LONG>(atom);
  prop->SetValue(ec, range, &var);
}

bool TextService::CompositionUsable(ITfContext* context) {
  if (!composition_) return true;
  ComPtr<ITfRange> range;
  if (compositionContext_.Get() == context && SUCCEEDED(composition_->GetRange(&range))) {
    return true;
  }
  ResetState();
  return false;
}

void TextService::UpdateComposition(ITfContext* context, TfEditCookie ec) {
  if (!CompositionUsable(context)) return;
  if (!IsComposing()) {
    CancelComposition(context, ec);
    return;
  }
  std::wstring display = composer_.Text();
  const size_t insertAt = composer_.TextOffsetOf(composer_.cursor());
  std::wstring readingText = reading_.Text();
  // An invalid first-tone syllable shows its tone mark so the highlight covers what was typed.
  if (readingInvalid_ && reading_.Current().tone() == 1) readingText += static_cast<wchar_t>(0x02C9);
  display.insert(std::min(insertAt, display.size()), readingText);
  const LONG caret = static_cast<LONG>(insertAt + readingText.size());

  if (!composition_ && !StartComposition(context, ec)) return;
  ComPtr<ITfRange> range;
  if (FAILED(composition_->GetRange(&range))) return;
  range->SetText(ec, 0, display.c_str(), static_cast<LONG>(display.size()));
  ApplyDisplayAttribute(context, ec, range.Get(), inputAttributeAtom_);
  if (readingInvalid_ && !readingText.empty()) {
    const LONG begin = static_cast<LONG>(insertAt);
    if (ComPtr<ITfRange> bad = SubRange(range.Get(), ec, begin, caret)) {
      ApplyDisplayAttribute(context, ec, bad.Get(), errorAttributeAtom_);
    }
  }
  SetCaret(context, ec, range.Get(), caret);
}

void TextService::CommitOverflow(ITfContext* context, TfEditCookie ec) {
  if (!CompositionUsable(context)) return;
  if (composer_.size() <= kMaxElements || composer_.cursor() < composer_.size()) return;
  size_t drop = 0;
  std::wstring committed;
  for (const Node& node : composer_.Walk()) {
    if (composer_.size() - drop <= kKeepElements) break;
    drop += node.length;
    committed += node.text;
  }
  if (drop == 0) return;

  // Write the full text, then move the composition start past the committed prefix.
  UpdateComposition(context, ec);
  ComPtr<ITfRange> range;
  if (!composition_ || FAILED(composition_->GetRange(&range))) return;
  const LONG len = static_cast<LONG>(committed.size());
  if (ComPtr<ITfRange> prefix = SubRange(range.Get(), ec, 0, len)) {
    ClearDisplayAttribute(context, ec, prefix.Get());
  }
  if (ComPtr<ITfRange> rest = SubRange(range.Get(), ec, len, len)) {
    composition_->ShiftStart(ec, rest.Get());
  }
  LearnFromCommit(drop);
  composer_.DropFront(drop);
}

void TextService::CommitAll(ITfContext* context, TfEditCookie ec) {
  if (!CompositionUsable(context)) return;
  CloseCandidates();
  if (composition_) {
    LearnFromCommit(composer_.size());
    const std::wstring text = composer_.Text();
    ComPtr<ITfRange> range;
    if (SUCCEEDED(composition_->GetRange(&range))) {
      range->SetText(ec, 0, text.c_str(), static_cast<LONG>(text.size()));
      ClearDisplayAttribute(context, ec, range.Get());
      SetCaret(context, ec, range.Get(), static_cast<LONG>(text.size()));
    }
    EndComposition(ec);
  }
  ResetState();
}

void TextService::InsertDirect(ITfContext* context, TfEditCookie ec, const std::wstring& text) {
  ComPtr<ITfInsertAtSelection> insert;
  ComPtr<ITfRange> range;
  if (FAILED(context->QueryInterface(IID_PPV_ARGS(&insert))) ||
      FAILED(insert->InsertTextAtSelection(ec, 0, text.c_str(), static_cast<LONG>(text.size()),
                                           &range)) ||
      !range) {
    return;
  }
  range->Collapse(ec, TF_ANCHOR_END);
  TF_SELECTION sel{};
  sel.range = range.Get();
  sel.style.ase = TF_AE_NONE;
  context->SetSelection(ec, 1, &sel);
}

void TextService::CancelComposition(ITfContext* context, TfEditCookie ec) {
  if (!CompositionUsable(context)) return;
  CloseCandidates();
  if (composition_) {
    ComPtr<ITfRange> range;
    if (SUCCEEDED(composition_->GetRange(&range))) {
      range->SetText(ec, 0, L"", 0);
      ClearDisplayAttribute(context, ec, range.Get());
    }
    EndComposition(ec);
  }
  ResetState();
}

void TextService::ResetState() {
  CloseCandidates();
  composer_.Clear();
  reading_.Clear();
  composition_.Reset();
  compositionContext_.Reset();
  ApplyUserDictionary();
}

STDMETHODIMP TextService::OnCompositionTerminated(TfEditCookie, ITfComposition* composition) {
  return Guarded("OnCompositionTerminated", HRESULT(E_FAIL), [&]() -> HRESULT {
    // The app ended the composition (e.g. focus moved); its text stays in the document. A
    // composition we already dropped may report late; it must not end the current one.
    if (composition && composition_ && composition != composition_.Get()) return S_OK;
    LearnFromCommit(composer_.size());
    ResetState();
    return S_OK;
  });
}

bool TextService::GetTextRect(ITfContext* context, TfEditCookie ec, size_t begin, size_t end,
                              RECT* rect) {
  ComPtr<ITfContextView> view;
  ComPtr<ITfRange> range;
  if (!composition_ || FAILED(context->GetActiveView(&view)) ||
      FAILED(composition_->GetRange(&range))) {
    return false;
  }
  ComPtr<ITfRange> sub =
      SubRange(range.Get(), ec, static_cast<LONG>(begin), static_cast<LONG>(end));
  BOOL clipped = FALSE;
  if (!sub || view->GetTextExt(ec, sub.Get(), rect, &clipped) != S_OK) return false;
  HWND hwnd = nullptr;
  view->GetWnd(&hwnd);
  ToPhysical(hwnd, rect);
  return true;
}

void TextService::OpenCandidates(ITfContext* context, TfEditCookie ec) {
  candidates_ = composer_.CandidatesAtCursor();
  if (candidates_.empty()) return;
  std::vector<std::wstring> items;
  items.reserve(candidates_.size());
  size_t start = candidates_.front().start;
  for (const Candidate& c : candidates_) {
    items.push_back(c.text);
    start = std::min(start, c.start);
  }
  const size_t begin = composer_.TextOffsetOf(start);
  RECT rc{};
  if (!GetTextRect(context, ec, begin, begin + 1, &rc)) {
    GUITHREADINFO gti{sizeof(gti)};
    if (GetGUIThreadInfo(0, &gti) && gti.hwndCaret) {
      rc = gti.rcCaret;
      MapWindowPoints(gti.hwndCaret, nullptr, reinterpret_cast<POINT*>(&rc), 2);
    } else {
      POINT pt;
      GetCursorPos(&pt);
      rc = {pt.x, pt.y, pt.x, pt.y};
    }
  }
  candidateWindow_->Show(std::move(items), rc, RequestCandidateProbabilities());
}

void TextService::SelectCandidate(ITfContext* context, TfEditCookie ec, size_t index) {
  if (index >= candidates_.size()) return;
  composer_.Select(candidates_[index]);
  CloseCandidates();
  UpdateComposition(context, ec);
  RequestRanking();
}

void TextService::OpenEmojiPanel() {
  if (compositionContext_) {
    ComPtr<ITfContext> context = compositionContext_;
    EditSession::Request(context.Get(), clientId_, TF_ES_SYNC | TF_ES_READWRITE,
                         [this, context](TfEditCookie ec) {
                           CommitAll(context.Get(), ec);
                           return S_OK;
                         });
  }
  CloseCandidates();
  // Win+. opens the emoji and symbol panel, which types into the focused app.
  INPUT inputs[4] = {};
  const WORD keys[2] = {VK_LWIN, VK_OEM_PERIOD};
  for (int i = 0; i < 2; ++i) {
    inputs[i].type = INPUT_KEYBOARD;
    inputs[i].ki.wVk = keys[i];
    inputs[3 - i].type = INPUT_KEYBOARD;
    inputs[3 - i].ki.wVk = keys[i];
    inputs[3 - i].ki.dwFlags = KEYEVENTF_KEYUP;
  }
  SendInput(4, inputs, sizeof(INPUT));
}

void TextService::CloseCandidates() {
  if (candidateWindow_) candidateWindow_->Hide();
  candidates_.clear();
  candidateRequestId_ = 0;
}

}  // namespace llaime
