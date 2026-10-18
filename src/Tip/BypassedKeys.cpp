// Keys the app handled without asking the text service.
//
// A TSF-aware app such as Word passes each key to TSF itself (ITfKeystrokeMgr), and it may
// decide not to: right after the clipboard changes, Word handles keys on its own until a
// character key is typed, so Ctrl+, after a copy and paste never reached the IME (and Word,
// seeing a lone Ctrl, opened its Paste Options instead). Microsoft's own IMEs are not
// affected; they take keys outside the app's message loop.
//
// TSF's key trace sees every key either way. A key the IME would have taken that the app did
// not offer it is done after the app has handled it:
//  - Ctrl+punctuation: the app typed nothing for it, so the punctuation is simply added;
//  - a character key (a Bopomofo key, punctuation): the app typed the character itself, so it
//    is taken back first, and only if the character right before the caret is that one.
#include "EditSession.h"
#include "Globals.h"
#include "KeyMapping.h"
#include "TextService.h"

#include <cwctype>

using Microsoft::WRL::ComPtr;

namespace llaime {
namespace {

constexpr wchar_t kWindowClass[] = L"LLaIME.KeyWatch";
constexpr UINT kCheckBypassed = WM_APP + 1;
constexpr UINT_PTR kRetryTimer = 1;
constexpr UINT kRetryMs = 60;

}  // namespace

LRESULT CALLBACK TextService::KeyWatchProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  if (msg == WM_TIMER && wParam == kRetryTimer) {
    KillTimer(hwnd, kRetryTimer);
    if (auto* self = reinterpret_cast<TextService*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA))) {
      Guarded("KeyWatchProc", [&] { self->DoBypassedKey(); });
    }
    return 0;
  }
  if (msg == kCheckBypassed) {
    auto* self = reinterpret_cast<TextService*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!self) return 0;
    if (wParam == 0) self->bypassedConfirmed_ = true;
    // The app turns a key it handles itself into a WM_CHAR posted while handling the key, after
    // this check was posted: look once more, after that character.
    if (wParam == 0 && self->bypassedPending_ && self->bypassed_.typed) {
      PostMessageW(hwnd, kCheckBypassed, 1, 0);
      return 0;
    }
    Guarded("KeyWatchProc", [&] { self->DoBypassedKey(); });
    return 0;
  }
  return DefWindowProcW(hwnd, msg, wParam, lParam);
}

bool TextService::InitKeyTrace() {
  WNDCLASSW wc{};
  wc.lpfnWndProc = KeyWatchProc;
  wc.hInstance = g_hInst;
  wc.lpszClassName = kWindowClass;
  RegisterClassW(&wc);  // fails harmlessly when already registered in this process
  keyWatchWindow_ = CreateWindowExW(0, kWindowClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                                    g_hInst, nullptr);
  if (!keyWatchWindow_) return false;
  SetWindowLongPtrW(keyWatchWindow_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
  ComPtr<ITfSource> source;
  return SUCCEEDED(threadMgr_.As(&source)) &&
         SUCCEEDED(source->AdviseSink(IID_ITfKeyTraceEventSink,
                                      static_cast<ITfKeyTraceEventSink*>(this),
                                      &keyTraceCookie_));
}

void TextService::UninitKeyTrace() {
  ComPtr<ITfSource> source;
  if (keyTraceCookie_ != TF_INVALID_COOKIE && threadMgr_ && SUCCEEDED(threadMgr_.As(&source))) {
    source->UnadviseSink(keyTraceCookie_);
  }
  keyTraceCookie_ = TF_INVALID_COOKIE;
  if (keyWatchWindow_) DestroyWindow(keyWatchWindow_);
  keyWatchWindow_ = nullptr;
  bypassedPending_ = false;
}

// Called for every key the IME is asked about (OnTestKeyDown / OnKeyDown).
void TextService::NoteKeyOffered(WPARAM vk) {
  lastOfferedVk_ = vk;
  // Offered while the app handles it after all; once the app is done with the key message
  // (bypassedConfirmed_), the same key again is a new press.
  if (bypassedPending_ && !bypassedConfirmed_ && bypassed_.vk == vk) bypassedPending_ = false;
}

STDMETHODIMP TextService::OnKeyTraceDown(WPARAM wParam, LPARAM) {
  return Guarded("OnKeyTraceDown", HRESULT(S_OK), [&]() -> HRESULT {
    // Apps that leave key handling to TSF offer the key to the IME before the trace sees it;
    // apps that drive TSF themselves (Word) offer it afterwards, while handling the message.
    const bool offeredFirst = lastOfferedVk_ == wParam;
    lastOfferedVk_ = 0;
    if (offeredFirst || !chineseMode_ || !keyWatchWindow_ || IsComposing()) return S_OK;
    const Modifiers mods = Modifiers::Current();
    if (mods.alt) return S_OK;
    KeyAction action = ClassifyKey(wParam);
    using Type = KeyAction::Type;
    if (action.type != Type::Literal && action.type != Type::DirectInsert &&
        action.type != Type::Reading) {
      return S_OK;
    }
    // What the app typed for the key itself (nothing with Ctrl held).
    wchar_t typed = 0;
    if (!mods.ctrl) {
      typed = UsCharFromVirtualKey(wParam, mods.shift);
      if ((GetKeyState(VK_CAPITAL) & 1) && typed >= L'a' && typed <= L'z') typed -= 0x20;
      if (!typed || typed == L' ') return S_OK;
    }
    // Checked once the app has handled this key message (posted messages come before the next
    // input; see KeyWatchProc).
    bypassed_ = {wParam, std::move(action), typed};
    bypassedPending_ = true;
    bypassedAttempts_ = 0;
    bypassedConfirmed_ = false;
    PostMessageW(keyWatchWindow_, kCheckBypassed, 0, 0);
    return S_OK;
  });
}

STDMETHODIMP TextService::OnKeyTraceUp(WPARAM, LPARAM) { return S_OK; }

// Removes `typed` right before an empty selection (the character the app typed for a key the
// IME should have had). False, touching nothing, if that is not what is there; letters are
// compared without case, as the app may have capitalized one at a sentence start.
bool TextService::TakeBackTyped(ITfContext* context, TfEditCookie ec, wchar_t typed) {
  TF_SELECTION sel{};
  ULONG fetched = 0;
  if (FAILED(context->GetSelection(ec, TF_DEFAULT_SELECTION, 1, &sel, &fetched)) || !fetched) {
    return false;
  }
  ComPtr<ITfRange> range;
  range.Attach(sel.range);
  BOOL empty = FALSE;
  if (FAILED(range->IsEmpty(ec, &empty)) || !empty) return false;
  LONG moved = 0;
  if (FAILED(range->ShiftStart(ec, -1, &moved, nullptr)) || moved != -1) return false;
  wchar_t text[2] = {};
  ULONG got = 0;
  if (FAILED(range->GetText(ec, 0, text, 1, &got)) || got != 1) return false;
  if (towlower(text[0]) != towlower(typed)) return false;
  return SUCCEEDED(range->SetText(ec, 0, L"", 0));
}

// Does the bypassed key in an edit session. A character the app typed may reach the document
// a little later (Word buffers typing), so it is looked for again a few times, and at the
// latest before the next key the IME handles (ApplyBypassedKey from ExecuteKeyAction).
void TextService::DoBypassedKey() {
  if (!bypassedPending_) return;  // the app did offer it
  ComPtr<ITfDocumentMgr> docMgr;
  ComPtr<ITfContext> context;
  if (!threadMgr_ || FAILED(threadMgr_->GetFocus(&docMgr)) || !docMgr ||
      FAILED(docMgr->GetTop(&context)) || !context) {
    bypassedPending_ = false;
    return;
  }
  ComPtr<TextService> self(this);  // the session may run after a deactivation
  EditSession::Request(context.Get(), clientId_, TF_ES_ASYNCDONTCARE | TF_ES_READWRITE,
                       [self, context](TfEditCookie ec) {
                         if (self->threadMgr_) self->ApplyBypassedKey(context.Get(), ec, false);
                         return S_OK;
                       });
  if (bypassedPending_ && keyWatchWindow_) {
    if (++bypassedAttempts_ < kBypassedAttempts) {
      SetTimer(keyWatchWindow_, kRetryTimer, kRetryMs, nullptr);
    } else {
      bypassedPending_ = false;  // the app did something else with the key
    }
  }
}

// `last`: no later attempt (the next key is being handled): give up if it cannot be done now.
void TextService::ApplyBypassedKey(ITfContext* context, TfEditCookie ec, bool last) {
  if (!bypassedPending_) return;
  if (IsComposing()) {
    bypassedPending_ = false;
    return;
  }
  if (bypassed_.typed && !TakeBackTyped(context, ec, bypassed_.typed)) {
    if (last) bypassedPending_ = false;
    return;
  }
  bypassedPending_ = false;
  HandleAction(context, ec, bypassed_.action);
}

}  // namespace llaime
