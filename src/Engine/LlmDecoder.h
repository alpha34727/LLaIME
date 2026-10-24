#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "LlmScorer.h"

namespace llaime {

// The language model writes the conversion itself instead of ranking the lexicon's N best:
// one vocabulary token at a time, only tokens whose every character is one the lexicon gives
// for the next syllables (Qwen's tokens are often whole words). Beam search over element
// positions: hypotheses covering the same elements compete, `beam` kept per position, one
// model call per position. A hypothesis scores
//   log P_model(text) + lexiconWeight * ln(10) * (lexicon's best segmentation of the text),
// so the lexicon still knows words the model does not prefer. Arcs that must be taken whole
// (pins, punctuation written with more than one character) are appended as they are; a
// character the model has no token of its own for (rare characters it splits into bytes) is
// filled in from the lexicon with its lexicon probability.
//
// Results for the positions of the previous call are kept: when the next call has the same
// context and the same lattice up to some position (typing one more syllable), only the
// positions after it are decoded again, about one model call per new syllable.
class LlmDecoder {
 public:
  struct Arc {
    size_t start = 0, length = 0;
    bool forced = false;
    double score = 0;  // log10
    std::wstring text;
  };
  struct Path {
    std::vector<uint32_t> arcs;  // indices into the lattice, in order
    double lm = 0;
    double lexicon = 0;
  };

  explicit LlmDecoder(LlmScorer* scorer);

  // Up to `beam` (at most 16) conversions of `elements` elements over `lattice`, best first.
  // `context` includes the prompt; `following` (UTF-8) is the text after the composition, which
  // rescores the final hypotheses when given.
  std::vector<Path> Decode(const std::string& context, const std::string& following,
                           size_t elements, const std::vector<Arc>& lattice, size_t beam,
                           double lexiconWeight);
  // Forgets the previous call (its text).
  void Clear() {
    cachedContext_.clear();
    cachedSignature_.clear();
    cachedPositions_.clear();
    cachedFinal_.clear();
  }

 private:
  struct Hyp {
    std::vector<int> tokens;
    std::wstring text;
    std::vector<int> offset;   // text offset at each element boundary so far; -1 inside a whole arc
    std::vector<double> best;  // lexicon log10 of the best segmentation up to each boundary
    std::vector<int> back;     // the arc ending there in that segmentation
    double lm = 0;
    double score = 0;
  };
  struct Position {
    std::vector<Hyp> hyps;  // kept after pruning, before expanding this position
    // For each kept hypothesis: tokens starting with a character allowed here, and their
    // log probability after it.
    std::vector<std::vector<std::pair<int, float>>> next;
  };

  void Prune(std::vector<Hyp>& hyps, size_t beam) const;
  // Appends `text` to `h` from element `from` (one element per character, or all of it as one
  // element) and extends its lexicon segmentation over it. The caller adds the tokens.
  void Extend(Hyp& h, size_t from, const std::wstring& text, const std::vector<Arc>& lattice,
              bool oneElement = false) const;
  void Rescore(Hyp& h, double lexiconWeight) const;

  LlmScorer* scorer_;
  std::vector<std::wstring> tokenTexts_;
  std::unordered_map<wchar_t, std::vector<int>> tokensByFirst_;
  std::unordered_set<wchar_t> singleTokenChars_;
  // Arcs ending at each boundary (for the segmentation), set per call.
  std::vector<std::vector<uint32_t>> endingAt_;

  // The previous call.
  std::string cachedContext_;
  double cachedWeight_ = -1;
  size_t cachedBeam_ = 0;
  std::vector<uint64_t> cachedSignature_;  // per element: the arcs ending at it
  std::vector<Position> cachedPositions_;  // expanded positions
  std::vector<Hyp> cachedFinal_;           // hypotheses reaching the end, before pruning
};

}  // namespace llaime
