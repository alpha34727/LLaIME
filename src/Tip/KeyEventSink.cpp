#include "EditSession.h"
#include "KeyMapping.h"
#include "TextService.h"
#include "ui/CandidateWindow.h"

using Microsoft::WRL::ComPtr;

namespace llaime {
namespace {

using Type = KeyAction::Type;

KeyAction Make(Type type) {
  KeyAction a;
  a.type = type;
  return a;
}

KeyAction MakeLiteral(std::wstring text) {
  KeyAction a = Make(Type::Literal);
  a.text = std::move(text);
  return a;
}

KeyAction MakeInsert(std::wstring text) {
  KeyAction a = Make(Type::DirectInsert);
  a.text = std::move(text);
  return a;
}

// First entry of the named lexicon tables `prefixes` + `usChar`.
std::wstring LookupPunctuation(const Lexicon* lexicon, wchar_t usChar,
                               std::initializer_list<const wchar_t*> prefixes) {
  if (!lexicon) return {};
  std::vector<LexEntry> entries;
  for (std::wstring prefix : prefixes) {
    if (lexicon->LookupNamed(prefix + usChar, entries)) return std::wstring(entries.front().text);
  }
  return {};
}

// Chinese punctuation for an unshifted non-Bopomofo key (e.g. ' -> 、, [ -> 「).
std::wstring Punctuation(const Lexicon* lexicon, wchar_t usChar) {
  return LookupPunctuation(lexicon, usChar, {L"_punctuation_Standard_", L"_punctuation_"});
}

// Ctrl+punctuation as in Microsoft Bopomofo: Ctrl+, ， Ctrl+. 。 Ctrl+/ ？ Ctrl+; ；
// Ctrl+' 、 Ctrl+Shift+1 ！, falling back to the Shift table (Ctrl+Shift+; ：, Ctrl+[ 「).
std::wstring CtrlPunctuation(const Lexicon* lexicon, wchar_t usChar) {
  return LookupPunctuation(lexicon, usChar, {L"_ctrl_punctuation_", L"_punctuation_"});
}

// ASCII to its full-width form; space becomes the ideographic space.
std::wstring ToFullWidth(wchar_t c) {
  if (c == L' ') return std::wstring(1, static_cast<wchar_t>(0x3000));
  if (c >= 0x21 && c <= 0x7e) return std::wstring(1, static_cast<wchar_t>(c + 0xfee0));
  return std::wstring(1, c);
}

}  // namespace

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

bool TextService::CandidatesOpen() const {
  return candidateWindow_ && candidateWindow_->IsVisible();
}

KeyAction TextService::ClassifyKey(WPARAM vk) const {
  const Modifiers mods = Modifiers::Current();
  if (vk == VK_SPACE && mods.shift && !mods.ctrl && !mods.alt) return Make(Type::ToggleShape);

  if (!chineseMode_) {
    // English mode only intercepts printable keys when typing full-width.
    if (!fullShape_ || mods.ctrl || mods.alt) return Make(Type::PassThrough);
    wchar_t ch = UsCharFromVirtualKey(vk, mods.shift);
    return ch ? MakeInsert(ToFullWidth(ch)) : Make(Type::PassThrough);
  }

  const bool composing = IsComposing();
  if (mods.alt) return Make(Type::PassThrough);
  if (mods.ctrl) {
    wchar_t ch = UsCharFromVirtualKey(vk, mods.shift);
    if (ch && !iswalnum(ch) && ch != L' ') {
      if (std::wstring punct = CtrlPunctuation(lexicon_, ch); !punct.empty()) {
        return MakeLiteral(punct);
      }
    }
    return Make(Type::PassThrough);
  }

  if (CandidatesOpen()) {
    if (vk >= '1' && vk <= '9' && !mods.shift) {
      KeyAction a = Make(Type::CandidateDigit);
      a.number = static_cast<int>(vk - '0');
      return a;
    }
    switch (vk) {
      case VK_UP: return Make(Type::CandidateUp);
      case VK_DOWN: return Make(Type::CandidateDown);
      case VK_PRIOR: case VK_LEFT: return Make(Type::CandidatePageUp);
      case VK_NEXT: case VK_RIGHT: case VK_SPACE: return Make(Type::CandidatePageDown);
      case VK_RETURN: return Make(Type::CandidateAccept);
      case VK_ESCAPE: return Make(Type::CandidateClose);
      default: break;  // any other key closes the list and is handled normally
    }
  }

  // Caps Lock types English letters: upper case, or lower case with Shift.
  const bool capsLock = (GetKeyState(VK_CAPITAL) & 1) != 0;
  if (capsLock && vk >= 'A' && vk <= 'Z') {
    wchar_t letter = static_cast<wchar_t>(mods.shift ? vk - 'A' + 'a' : vk);
    return MakeLiteral(fullShape_ ? ToFullWidth(letter) : std::wstring(1, letter));
  }

  const wchar_t ch = UsCharFromVirtualKey(vk, mods.shift);
  if (ch && !mods.shift) {
    if (ch == L' ' && reading_.Empty()) {
      // Space types a space, inside the composition if one is in progress.
      std::wstring space = fullShape_ ? ToFullWidth(L' ') : L" ";
      if (composing) return MakeLiteral(space);
      return fullShape_ ? MakeInsert(space) : Make(Type::PassThrough);
    }
    if (reading_.IsToneKey(ch) && reading_.Empty()) {
      // A tone with nothing to apply to: type the digit unless we are composing.
      return composing ? Make(Type::Swallow) : Make(Type::PassThrough);
    }
    if (reading_.IsLayoutKey(ch)) {
      KeyAction a = Make(Type::Reading);
      a.ch = ch;
      return a;
    }
  }
  if (ch && ch != L' ') {
    // Shift+key types what a US keyboard types (Shift+/ ?, Shift+' "), like Microsoft
    // Bopomofo; Ctrl+key gives Chinese punctuation instead.
    if (mods.shift) return MakeLiteral(fullShape_ ? ToFullWidth(ch) : std::wstring(1, ch));
    if (std::wstring punct = Punctuation(lexicon_, ch); !punct.empty()) return MakeLiteral(punct);
  }

  if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9 && (composing || fullShape_)) {
    wchar_t digit = static_cast<wchar_t>(L'0' + (vk - VK_NUMPAD0));
    std::wstring text = fullShape_ ? ToFullWidth(digit) : std::wstring(1, digit);
    return composing ? MakeLiteral(text) : MakeInsert(text);
  }
  if (!composing) return Make(Type::PassThrough);
  switch (vk) {
    case VK_BACK: return Make(Type::Backspace);
    case VK_DELETE: return Make(Type::Delete);
    case VK_LEFT: return Make(Type::MoveLeft);
    case VK_RIGHT: return Make(Type::MoveRight);
    case VK_HOME: return Make(Type::MoveHome);
    case VK_END: return Make(Type::MoveEnd);
    case VK_RETURN: return Make(Type::Commit);
    case VK_ESCAPE: return Make(Type::Cancel);
    case VK_DOWN: return reading_.Empty() ? Make(Type::OpenCandidates) : Make(Type::Swallow);
    case VK_UP: case VK_PRIOR: case VK_NEXT: case VK_TAB: return Make(Type::Swallow);
    default: break;
  }
  return Make(Type::PassThrough);
}

HRESULT TextService::ExecuteKeyAction(ITfContext* context, const KeyAction& action) {
  ComPtr<ITfContext> ctx(context);
  return EditSession::Request(context, clientId_, TF_ES_SYNC | TF_ES_READWRITE,
                              [this, ctx, action](TfEditCookie ec) {
                                HandleAction(ctx.Get(), ec, action);
                                return S_OK;
                              });
}

void TextService::HandleAction(ITfContext* context, TfEditCookie ec, const KeyAction& action) {
  // Keys that are not for the candidate list close it first.
  const bool candidateKey =
      action.type >= Type::CandidateDigit && action.type <= Type::CandidateClose;
  if (!candidateKey) CloseCandidates();
  // The invalid-syllable highlight lasts until the next key.
  if (action.type != Type::Swallow) readingInvalid_ = false;

  switch (action.type) {
    case Type::PassThrough:
    case Type::Swallow:
      return;
    case Type::Reading:
      if (reading_.Feed(action.ch) == BopomofoReading::Result::Completed) {
        Syllable s = reading_.TakeSyllable();
        if (lexicon_ && !lexicon_->Contains(std::span<const Syllable>(&s, 1))) {
          // Not a real syllable: keep it (with its tone) highlighted for correction and beep,
          // like Microsoft Bopomofo. Another tone key retries; Backspace removes the tone.
          reading_.Restore(s);
          readingInvalid_ = true;
          MessageBeep(MB_OK);
        } else {
          composer_.InsertSyllable(s);
          CommitOverflow(context, ec);
        }
      }
      break;
    case Type::Literal:
      reading_.Clear();
      composer_.InsertLiteral(action.text);
      CommitOverflow(context, ec);
      break;
    case Type::Backspace:
      if (!reading_.Backspace()) composer_.DeleteBefore();
      break;
    case Type::Delete:
      if (reading_.Empty()) composer_.DeleteAfter();
      break;
    case Type::MoveLeft:
      if (reading_.Empty() && composer_.cursor() > 0) composer_.SetCursor(composer_.cursor() - 1);
      break;
    case Type::MoveRight:
      if (reading_.Empty()) composer_.SetCursor(composer_.cursor() + 1);
      break;
    case Type::MoveHome:
      if (reading_.Empty()) composer_.SetCursor(0);
      break;
    case Type::MoveEnd:
      if (reading_.Empty()) composer_.SetCursor(composer_.size());
      break;
    case Type::Commit:
      CommitAll(context, ec);
      return;
    case Type::Cancel:
      if (!reading_.Empty()) {
        reading_.Clear();
      } else {
        CancelComposition(context, ec);
        return;
      }
      break;
    case Type::OpenCandidates:
      OpenCandidates(context, ec);
      return;
    case Type::CandidateDigit:
      if (auto index = candidateWindow_->IndexForDigit(action.number)) {
        SelectCandidate(context, ec, *index);
      }
      return;
    case Type::CandidateUp:
      candidateWindow_->MoveSelection(-1);
      return;
    case Type::CandidateDown:
      candidateWindow_->MoveSelection(1);
      return;
    case Type::CandidatePageUp:
      candidateWindow_->MovePage(-1);
      return;
    case Type::CandidatePageDown:
      candidateWindow_->MovePage(1);
      return;
    case Type::CandidateAccept:
      SelectCandidate(context, ec, candidateWindow_->Selection());
      return;
    case Type::CandidateClose:
      CloseCandidates();
      return;
    case Type::ToggleShape:
      SetFullShape(!fullShape_);
      return;
    case Type::DirectInsert:
      InsertDirect(context, ec, action.text);
      return;
  }
  UpdateComposition(context, ec);
}

STDMETHODIMP TextService::OnSetFocus(BOOL) { return S_OK; }

STDMETHODIMP TextService::OnTestKeyDown(ITfContext*, WPARAM wParam, LPARAM, BOOL* eaten) {
  shiftAlone_ = wParam == VK_SHIFT;
  *eaten = ClassifyKey(wParam).type != Type::PassThrough;
  return S_OK;
}

STDMETHODIMP TextService::OnKeyDown(ITfContext* context, WPARAM wParam, LPARAM, BOOL* eaten) {
  shiftAlone_ = wParam == VK_SHIFT;
  KeyAction action = ClassifyKey(wParam);
  *eaten = action.type != Type::PassThrough;
  if (*eaten) ExecuteKeyAction(context, action);
  return S_OK;
}

// A Shift press and release with no other key in between toggles Chinese/English.
STDMETHODIMP TextService::OnTestKeyUp(ITfContext*, WPARAM wParam, LPARAM, BOOL* eaten) {
  *eaten = wParam == VK_SHIFT && shiftAlone_;
  return S_OK;
}

STDMETHODIMP TextService::OnKeyUp(ITfContext* context, WPARAM wParam, LPARAM, BOOL* eaten) {
  *eaten = FALSE;
  if (wParam != VK_SHIFT || !shiftAlone_) return S_OK;
  shiftAlone_ = false;
  *eaten = TRUE;
  if (chineseMode_ && IsComposing() && context) {
    ComPtr<ITfContext> ctx(context);
    EditSession::Request(context, clientId_, TF_ES_SYNC | TF_ES_READWRITE,
                         [this, ctx](TfEditCookie ec) {
                           CommitAll(ctx.Get(), ec);
                           return S_OK;
                         });
  }
  SetChineseMode(!chineseMode_);
  return S_OK;
}

STDMETHODIMP TextService::OnPreservedKey(ITfContext*, REFGUID, BOOL* eaten) {
  *eaten = FALSE;
  return S_OK;
}

}  // namespace llaime
