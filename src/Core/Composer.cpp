#include "llaime/Composer.h"

#include <algorithm>
#include <limits>

namespace llaime {
namespace {

// Score for text that the lexicon cannot convert; low enough that any real phrase wins.
constexpr double kUnknownScore = -50.0;

}  // namespace

void Composer::SetCursor(size_t pos) { cursor_ = std::min(pos, elements_.size()); }

void Composer::InsertSyllable(Syllable s) {
  elements_.insert(elements_.begin() + cursor_, Element{s, {}});
  OnInserted(cursor_);
  ++cursor_;
}

void Composer::InsertLiteral(std::wstring text) {
  elements_.insert(elements_.begin() + cursor_, Element{Syllable(), std::move(text)});
  OnInserted(cursor_);
  ++cursor_;
}

bool Composer::DeleteBefore() {
  if (cursor_ == 0) return false;
  --cursor_;
  elements_.erase(elements_.begin() + cursor_);
  OnErased(cursor_);
  return true;
}

bool Composer::DeleteAfter() {
  if (cursor_ >= elements_.size()) return false;
  elements_.erase(elements_.begin() + cursor_);
  OnErased(cursor_);
  return true;
}

void Composer::Clear() {
  elements_.clear();
  pins_.clear();
  cursor_ = 0;
  Invalidate();
}

void Composer::DropFront(size_t count) {
  count = std::min(count, elements_.size());
  elements_.erase(elements_.begin(), elements_.begin() + count);
  std::erase_if(pins_, [&](const Pin& p) { return p.start < count; });
  for (Pin& p : pins_) p.start -= count;
  cursor_ = cursor_ > count ? cursor_ - count : 0;
  Invalidate();
}

void Composer::OnInserted(size_t pos) {
  // A pin split by the insertion no longer describes a contiguous reading.
  std::erase_if(pins_, [&](const Pin& p) { return p.start < pos && pos < p.start + p.length; });
  for (Pin& p : pins_) {
    if (p.start >= pos) ++p.start;
  }
  Invalidate();
}

void Composer::OnErased(size_t pos) {
  std::erase_if(pins_, [&](const Pin& p) { return p.start <= pos && pos < p.start + p.length; });
  for (Pin& p : pins_) {
    if (p.start > pos) --p.start;
  }
  Invalidate();
}

bool Composer::SpanHasLiteral(size_t start, size_t length) const {
  for (size_t i = start; i < start + length; ++i) {
    if (IsLiteral(i)) return true;
  }
  return false;
}

const Composer::Pin* Composer::PinFor(size_t start, size_t length, bool* conflicts) const {
  *conflicts = false;
  for (const Pin& p : pins_) {
    if (p.start == start && p.length == length) return &p;
    bool overlap = p.start < start + length && start < p.start + p.length;
    if (overlap) *conflicts = true;
  }
  return nullptr;
}

std::vector<Syllable> Composer::Syllables(size_t start, size_t length) const {
  std::vector<Syllable> out;
  for (size_t i = start; i < start + length && i < elements_.size(); ++i) {
    out.push_back(elements_[i].syllable);
  }
  return out;
}

const std::vector<Node>& Composer::Walk() const {
  if (walkValid_) return walk_;
  const size_t n = elements_.size();
  constexpr double kNegInf = -std::numeric_limits<double>::infinity();
  std::vector<double> best(n + 1, kNegInf);
  std::vector<Node> back(n + 1);
  best[0] = 0;

  std::vector<LexEntry> entries;
  for (size_t i = 0; i < n; ++i) {
    if (best[i] == kNegInf) continue;
    const size_t maxLen = std::min(Lexicon::kMaxPhraseLength, n - i);
    for (size_t len = 1; len <= maxLen; ++len) {
      Node node{i, len, {}, 0, false};
      bool conflicts = false;
      const Pin* pin = PinFor(i, len, &conflicts);
      if (pin) {
        node.text = pin->text;
        node.score = 0;
        node.pinned = true;
      } else if (conflicts) {
        continue;
      } else if (IsLiteral(i)) {
        if (len != 1) break;
        node.text = elements_[i].literal;
      } else if (SpanHasLiteral(i, len)) {
        break;
      } else {
        entries.clear();
        auto seq = Syllables(i, len);
        if (lexicon_ && lexicon_->Lookup(seq, entries)) {
          node.text = entries.front().text;
          node.score = entries.front().score;
        } else if (len == 1) {
          node.text = elements_[i].syllable.ToString();
          node.score = kUnknownScore;
        } else {
          continue;
        }
      }
      double total = best[i] + node.score;
      if (total > best[i + len]) {
        best[i + len] = total;
        back[i + len] = std::move(node);
      }
    }
  }

  walk_.clear();
  for (size_t pos = n; pos > 0 && back[pos].length > 0; pos -= back[pos].length) {
    walk_.push_back(back[pos]);
  }
  std::reverse(walk_.begin(), walk_.end());
  walkValid_ = true;
  return walk_;
}

std::wstring Composer::Text() const {
  std::wstring text;
  for (const Node& node : Walk()) text += node.text;
  return text;
}

size_t Composer::TextOffsetOf(size_t index) const {
  size_t offset = 0;
  for (const Node& node : Walk()) {
    if (index <= node.start) break;
    if (index >= node.start + node.length) {
      offset += node.text.size();
    } else {
      // Inside a node: assume one UTF-16 unit per element when the lengths match.
      size_t within = index - node.start;
      offset += node.text.size() == node.length ? within : node.text.size();
      break;
    }
  }
  return offset;
}

std::vector<Candidate> Composer::CandidatesAtCursor() const {
  std::vector<Candidate> out;
  const size_t n = elements_.size();
  if (n == 0 || !lexicon_) return out;
  const bool atEnd = cursor_ >= n;
  const size_t anchor = atEnd ? n : cursor_;

  std::vector<LexEntry> entries;
  for (size_t len = std::min(Lexicon::kMaxPhraseLength, n); len >= 1; --len) {
    size_t start;
    if (atEnd) {
      if (len > anchor) continue;
      start = anchor - len;
    } else {
      if (anchor + len > n) continue;
      start = anchor;
    }
    if (SpanHasLiteral(start, len)) continue;
    entries.clear();
    if (!lexicon_->Lookup(Syllables(start, len), entries)) continue;
    for (const LexEntry& e : entries) {
      bool dup = std::any_of(out.begin(), out.end(),
                             [&](const Candidate& c) { return c.text == e.text; });
      if (!dup) out.push_back({std::wstring(e.text), start, len, e.score});
    }
  }
  return out;
}

void Composer::Select(const Candidate& c) {
  std::erase_if(pins_, [&](const Pin& p) {
    return p.start < c.start + c.length && c.start < p.start + p.length;
  });
  pins_.push_back({c.start, c.length, c.text});
  Invalidate();
}

}  // namespace llaime
