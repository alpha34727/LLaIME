#pragma once
#include <string>
#include <vector>

#include "llaime/Composer.h"
#include "llaime/UserDictionary.h"

namespace llaime {

// What committing a conversion teaches (nothing is learned in private fields):
//  - each candidate the user picked, with the word before it;
//  - a run of two or more picks in a row whose joined text the lexicon does not have as one
//    phrase (a name picked character by character): repeated, it becomes a learned phrase.
struct LearningEvents {
  struct Selection {
    std::vector<Syllable> reading;
    std::wstring text;
    std::wstring prev;  // the word before it in the conversion, "" at the start
  };
  struct Phrase {
    std::vector<Syllable> reading;
    std::wstring text;
  };
  std::vector<Selection> selections;
  std::vector<Phrase> phrases;
  bool empty() const { return selections.empty() && phrases.empty(); }
};

// Events from the nodes of `composer.Walk()` that end at or before element `upTo` (the part
// being committed).
LearningEvents CollectLearning(const Composer& composer, size_t upTo);

// Ranking bonus (natural log, added to the combined score) for a sentence of `composer`'s
// buffer: the user's picks among its words, overall and after the same preceding word. Weights
// count picks with time decay and saturate at 1. Tuned with LLaIME.LearnSim
// (tests/eval/RESULTS.md): a strong bonus after the same word, a light one anywhere else.
inline constexpr double kPickBonus = 1.0;
inline constexpr double kPickContextBonus = 3.0;
double LearnedBonus(const UserDictionary& user, const Composer& composer,
                    const std::vector<Node>& nodes, double pickBonus = kPickBonus,
                    double contextBonus = kPickContextBonus);

}  // namespace llaime
