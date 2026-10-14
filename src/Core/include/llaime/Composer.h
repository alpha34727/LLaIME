#pragma once
#include <string>
#include <vector>

#include "llaime/Lexicon.h"
#include "llaime/Syllable.h"

namespace llaime {

// One segment of the converted sentence.
struct Node {
  size_t start = 0;   // index into the composer's elements
  size_t length = 0;  // number of elements covered
  std::wstring text;
  double score = 0;
  bool pinned = false;
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
  std::wstring Text() const;
  // Character offset in Text() of the boundary before element `index`.
  size_t TextOffsetOf(size_t index) const;

  // Candidates for the cursor position: phrases that start at the cursor, or end at it when the
  // cursor is at the end, longest first.
  std::vector<Candidate> CandidatesAtCursor() const;
  void Select(const Candidate& c);

  // Reading of the syllables in [start, start+length), for display and learning.
  std::vector<Syllable> Syllables(size_t start, size_t length) const;

 private:
  struct Element {
    Syllable syllable;
    std::wstring literal;  // non-empty for punctuation and other fixed text
  };
  struct Pin {
    size_t start, length;
    std::wstring text;
  };

  bool IsLiteral(size_t i) const { return !elements_[i].literal.empty(); }
  bool SpanHasLiteral(size_t start, size_t length) const;
  // Pin covering exactly this span, or that conflicts with it (overlaps but differs).
  const Pin* PinFor(size_t start, size_t length, bool* conflicts) const;
  void OnInserted(size_t pos);
  void OnErased(size_t pos);
  void Invalidate() { walkValid_ = false; }

  const Lexicon* lexicon_;
  std::vector<Element> elements_;
  std::vector<Pin> pins_;
  size_t cursor_ = 0;

  mutable std::vector<Node> walk_;
  mutable bool walkValid_ = false;
};

}  // namespace llaime
