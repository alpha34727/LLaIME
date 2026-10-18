// User phrase shortcuts, as in Microsoft Bopomofo: typing ` then p enters shortcut mode, and
// the keys that follow pick a phrase from the user dictionary (` p m k -> 「」, if "mk" was
// given to 「」 in the settings). The keys show underlined after the ` like a reading. As soon
// as the keys name a phrase and no longer shortcut starts with them, the phrase replaces the
// ` (and is committed if nothing else was being composed); Enter or Space takes an exact match
// when a longer shortcut also exists. A key that matches no shortcut beeps and is ignored.
#include "KeyMapping.h"
#include "TextService.h"

namespace llaime {
namespace {

using Type = KeyAction::Type;

KeyAction Make(Type type, wchar_t ch = 0) {
  KeyAction a;
  a.type = type;
  a.ch = ch;
  return a;
}

}  // namespace

// The ` (half or full width) right before the cursor, with no reading in progress.
bool TextService::AtShortcutTrigger() const {
  if (!reading_.Empty() || CandidatesOpen()) return false;
  const std::wstring_view before = composer_.LiteralBeforeCursor();
  return before == L"`" || before == L"\xFF40";
}

KeyAction TextService::ClassifyShortcutKey(WPARAM vk) const {
  if (vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU || vk == VK_CAPITAL) {
    return Make(Type::PassThrough);
  }
  const Modifiers mods = Modifiers::Current();
  if (!mods.ctrl && !mods.alt) {
    if (vk >= 'A' && vk <= 'Z') return Make(Type::ShortcutKey, static_cast<wchar_t>(vk - 'A' + 'a'));
    if (vk >= '0' && vk <= '9' && !mods.shift) return Make(Type::ShortcutKey, static_cast<wchar_t>(vk));
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) {
      return Make(Type::ShortcutKey, static_cast<wchar_t>(L'0' + (vk - VK_NUMPAD0)));
    }
  }
  switch (vk) {
    case VK_BACK: return Make(Type::ShortcutBackspace);
    case VK_RETURN: case VK_SPACE: return Make(Type::ShortcutAccept);
    case VK_ESCAPE: return Make(Type::ShortcutCancel);
    default: return Make(Type::ShortcutReject);
  }
}

void TextService::HandleShortcut(ITfContext* context, TfEditCookie ec, const KeyAction& action) {
  auto apply = [&](const std::wstring& text) {
    shortcutMode_ = false;
    shortcutKeys_.clear();
    composer_.DeleteBefore();  // the `
    const bool alone = composer_.empty();
    composer_.InsertLiteral(text);
    if (alone) {
      CommitAll(context, ec);
    } else {
      UpdateComposition(context, ec);
    }
  };
  switch (action.type) {
    case Type::ShortcutStart:
      shortcutMode_ = true;
      shortcutKeys_.clear();
      break;
    case Type::ShortcutKey: {
      const std::wstring keys = shortcutKeys_ + action.ch;
      const auto match = userDict_ ? userDict_->MatchShortcut(keys) : UserDictionary::ShortcutMatch{};
      if (!match.any() || keys.size() > UserDictionary::kMaxShortcutLength) {
        MessageBeep(MB_OK);
        break;
      }
      shortcutKeys_ = keys;
      if (match.text && !match.longer) {
        apply(*match.text);
        return;
      }
      break;
    }
    case Type::ShortcutBackspace:
      if (shortcutKeys_.empty()) {
        shortcutMode_ = false;  // back to the ` alone
      } else {
        shortcutKeys_.pop_back();
      }
      break;
    case Type::ShortcutAccept: {
      const auto match = userDict_ ? userDict_->MatchShortcut(shortcutKeys_)
                                   : UserDictionary::ShortcutMatch{};
      if (!shortcutKeys_.empty() && match.text) {
        apply(*match.text);
        return;
      }
      MessageBeep(MB_OK);
      break;
    }
    case Type::ShortcutCancel:
      shortcutMode_ = false;
      shortcutKeys_.clear();
      composer_.DeleteBefore();
      if (composer_.empty()) {
        CancelComposition(context, ec);
        return;
      }
      break;
    default:
      MessageBeep(MB_OK);
      break;
  }
  UpdateComposition(context, ec);
}

}  // namespace llaime
