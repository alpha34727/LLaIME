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

bool Composer::Lookup(std::span<const Syllable> seq, std::vector<LexEntry>& out) const {
  if (user_) return user_->Lookup(lexicon_, seq, out);
  out.clear();
  return lexicon_ && lexicon_->Lookup(seq, out);
}

std::vector<Syllable> Composer::Syllables(size_t start, size_t length) const {
  std::vector<Syllable> out;
  for (size_t i = start; i < start + length && i < elements_.size(); ++i) {
    out.push_back(elements_[i].syllable);
  }
  return out;
}

Composer::SpanResult Composer::SpanOptions(size_t i, size_t len, size_t maxOptions,
                                           std::vector<Node>& out) const {
  out.clear();
  bool conflicts = false;
  if (const Pin* pin = PinFor(i, len, &conflicts)) {
    out.push_back({i, len, pin->text, 0, true});
    return SpanResult::Ok;
  }
  if (conflicts) return SpanResult::Skip;
  if (IsLiteral(i)) {
    if (len != 1) return SpanResult::Stop;
    out.push_back({i, 1, elements_[i].literal, 0, false});
    return SpanResult::Ok;
  }
  if (SpanHasLiteral(i, len)) return SpanResult::Stop;
  std::vector<LexEntry> entries;
  if (Lookup(Syllables(i, len), entries)) {
    for (size_t k = 0; k < entries.size() && k < maxOptions; ++k) {
      out.push_back({i, len, std::wstring(entries[k].text), entries[k].score, false});
    }
    return SpanResult::Ok;
  }
  if (len == 1) {
    out.push_back({i, 1, elements_[i].syllable.ToString(), kUnknownScore, false});
    return SpanResult::Ok;
  }
  return SpanResult::Skip;
}

const std::vector<Node>& Composer::Walk() const {
  if (walkValid_) return walk_;
  const size_t n = elements_.size();
  constexpr double kNegInf = -std::numeric_limits<double>::infinity();
  std::vector<double> best(n + 1, kNegInf);
  std::vector<Node> back(n + 1);
  best[0] = 0;

  std::vector<Node> options;
  for (size_t i = 0; i < n; ++i) {
    if (best[i] == kNegInf) continue;
    const size_t maxLen = std::min(Lexicon::kMaxPhraseLength, n - i);
    for (size_t len = 1; len <= maxLen; ++len) {
      SpanResult r = SpanOptions(i, len, 1, options);
      if (r == SpanResult::Stop) break;
      if (r == SpanResult::Skip) continue;
      double total = best[i] + options.front().score;
      if (total > best[i + len]) {
        best[i + len] = total;
        back[i + len] = std::move(options.front());
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

std::vector<Sentence> Composer::NBest(size_t k, size_t perSpan) const {
  // k-best Viterbi: every boundary keeps its k best partial paths; a path is a node plus a
  // pointer to the path it extends (boundary, rank).
  struct Partial {
    double score;
    Node node;
    size_t prevPos;
    size_t prevRank;
  };
  const size_t n = elements_.size();
  std::vector<std::vector<Partial>> paths(n + 1);
  paths[0].push_back({0, Node{}, 0, 0});

  std::vector<Node> options;
  for (size_t i = 0; i < n; ++i) {
    if (paths[i].empty()) continue;
    const size_t maxLen = std::min(Lexicon::kMaxPhraseLength, n - i);
    for (size_t len = 1; len <= maxLen; ++len) {
      SpanResult r = SpanOptions(i, len, perSpan, options);
      if (r == SpanResult::Stop) break;
      if (r == SpanResult::Skip) continue;
      auto& target = paths[i + len];
      for (size_t rank = 0; rank < paths[i].size(); ++rank) {
        for (const Node& option : options) {
          target.push_back({paths[i][rank].score + option.score, option, i, rank});
        }
      }
      // Keep the k best so far; the target is final once i reaches it.
      if (target.size() > 4 * k) {
        std::partial_sort(target.begin(), target.begin() + k, target.end(),
                          [](const Partial& a, const Partial& b) { return a.score > b.score; });
        target.resize(k);
      }
    }
    if (i + 1 <= n) {
      auto& next = paths[i + 1];
      std::sort(next.begin(), next.end(),
                [](const Partial& a, const Partial& b) { return a.score > b.score; });
      if (next.size() > k) next.resize(k);
    }
  }

  std::vector<Sentence> out;
  for (size_t rank = 0; rank < paths[n].size() && out.size() < k; ++rank) {
    Sentence sentence;
    sentence.score = paths[n][rank].score;
    size_t pos = n, r = rank;
    while (pos > 0) {
      const Partial& p = paths[pos][r];
      sentence.nodes.push_back(p.node);
      pos = p.prevPos;
      r = p.prevRank;
    }
    std::reverse(sentence.nodes.begin(), sentence.nodes.end());
    // Different segmentations can spell the same text; keep the best-scoring one.
    const std::wstring text = sentence.Text();
    bool duplicate = std::any_of(out.begin(), out.end(),
                                 [&](const Sentence& s) { return s.Text() == text; });
    if (!duplicate) out.push_back(std::move(sentence));
  }
  return out;
}

void Composer::SetPreferred(const Sentence& sentence) {
  walk_ = sentence.nodes;
  walkValid_ = true;
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
  if (n == 0 || (!lexicon_ && !user_)) return out;
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
    if (!Lookup(Syllables(start, len), entries)) continue;
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
