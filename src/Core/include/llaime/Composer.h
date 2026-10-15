#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "llaime/Lexicon.h"
#include "llaime/Syllable.h"
#include "llaime/UserDictionary.h"

namespace llaime {

// One segment of the converted sentence.
struct Node {
  size_t start = 0;   // index into the composer's elements
  size_t length = 0;  // number of elements covered
  std::wstring text;
  double score = 0;
  bool pinned = false;
};

// A whole conversion of the buffer: one of the N best segmentations.
struct Sentence {
  std::vector<Node> nodes;
  double score = 0;  // sum of node log10 scores
  std::wstring Text() const {
    std::wstring text;
    for (const Node& node : nodes) text += node.text;
    return text;
  }
};

struct Candidate {
  std::wstring text;
  size_t start = 0;
  size_t length = 0;
  double score = 0;
};

// Holds the syllables typed so far and converts them to text by finding the best-scoring
// segmentation over the lexicon (Viterbi on log probabilities). Candidates chosen by the user
// are pinned and constrain later conversions.
class Composer {
 public:
  explicit Composer(const Lexicon* lexicon) : lexicon_(lexicon) {}

  // Replacing the lexicon (layers turned on or off) re-converts the buffer.
  void SetLexicon(const Lexicon* lexicon) {
    lexicon_ = lexicon;
    Invalidate();
  }

  // The user's phrases and picks, merged into every lookup (may be null). Must outlive its use
  // here; replacing it re-converts the buffer.
  void SetUserDictionary(const UserDictionary* user) {
    user_ = user;
    Invalidate();
  }

  size_t size() const { return elements_.size(); }
  bool empty() const { return elements_.empty(); }
  size_t cursor() const { return cursor_; }
  void SetCursor(size_t pos);

  void InsertSyllable(Syllable s);
  void InsertLiteral(std::wstring text);
  bool DeleteBefore();  // Backspace
  bool DeleteAfter();   // Delete
  void Clear();

  // Removes the first `count` elements (after they were committed to the document).
  void DropFront(size_t count);

  const std::vector<Node>& Walk() const;
  // The `k` best distinct conversions, considering the `perSpan` best entries of each span.
  // Pins apply as in Walk(). Used to give the language model alternatives to rank.
  std::vector<Sentence> NBest(size_t k, size_t perSpan = 4) const;
  std::wstring Text() const;

  // Bumped by every edit; a ranking result applies only to the generation it was computed for.
  uint64_t generation() const { return generation_; }
  // Shows `sentence` (one of NBest()) as the conversion until the next edit. Unlike Select(),
  // nothing is pinned: further typing is converted afresh.
  void SetPreferred(const Sentence& sentence);

  // Character offset in Text() of the boundary before element `index`.
  size_t TextOffsetOf(size_t index) const;

  // Candidates for the cursor position: phrases that start at the cursor, or end at it when the
  // cursor is at the end, longest first.
  std::vector<Candidate> CandidatesAtCursor() const;
  void Select(const Candidate& c);

  // Reading of the syllables in [start, start+length), for display and learning.
  std::vector<Syllable> Syllables(size_t start, size_t length) const;
  // Whether [start, start+length) holds only syllables (no punctuation or other literals).
  bool IsReadingSpan(size_t start, size_t length) const {
    return start + length <= elements_.size() && !SpanHasLiteral(start, length);
  }
  // Lexicon entries merged with the user dictionary's.
  bool Lookup(std::span<const Syllable> seq, std::vector<LexEntry>& out) const;

 private:
  struct Element {
    Syllable syllable;
    std::wstring literal;  // non-empty for punctuation and other fixed text
  };
  struct Pin {
    size_t start, length;
    std::wstring text;
  };

  enum class SpanResult { Ok, Skip, Stop };  // Stop: no longer span from here can match
  SpanResult SpanOptions(size_t i, size_t len, size_t maxOptions, std::vector<Node>& out) const;

  bool IsLiteral(size_t i) const { return !elements_[i].literal.empty(); }
  bool SpanHasLiteral(size_t start, size_t length) const;
  // Pin covering exactly this span, or that conflicts with it (overlaps but differs).
  const Pin* PinFor(size_t start, size_t length, bool* conflicts) const;
  void OnInserted(size_t pos);
  void OnErased(size_t pos);
  void Invalidate() {
    walkValid_ = false;
    ++generation_;
  }

  const Lexicon* lexicon_;
  const UserDictionary* user_ = nullptr;
  std::vector<Element> elements_;
  std::vector<Pin> pins_;
  size_t cursor_ = 0;

  uint64_t generation_ = 0;
  mutable std::vector<Node> walk_;
  mutable bool walkValid_ = false;
};

}  // namespace llaime
