#include "llaime/Learning.h"

#include <algorithm>

namespace llaime {

LearningEvents CollectLearning(const Composer& composer, size_t upTo) {
  LearningEvents events;
  const std::vector<Node>& nodes = composer.Walk();
  // A run of consecutive picks, as node indices [runStart, i).
  size_t runStart = 0;
  auto closeRun = [&](size_t end) {
    if (end - runStart < 2) return;
    const size_t start = nodes[runStart].start;
    const size_t length = nodes[end - 1].start + nodes[end - 1].length - start;
    if (length > Lexicon::kMaxPhraseLength || !composer.IsReadingSpan(start, length)) return;
    std::wstring text;
    for (size_t k = runStart; k < end; ++k) text += nodes[k].text;
    std::vector<Syllable> reading = composer.Syllables(start, length);
    std::vector<LexEntry> entries;
    composer.Lookup(reading, entries);
    const bool known = std::any_of(entries.begin(), entries.end(),
                                   [&](const LexEntry& e) { return e.text == text; });
    if (!known) events.phrases.push_back({std::move(reading), std::move(text)});
  };

  size_t i = 0;
  for (; i < nodes.size() && nodes[i].start + nodes[i].length <= upTo; ++i) {
    const Node& node = nodes[i];
    const bool picked = node.pinned && composer.IsReadingSpan(node.start, node.length);
    if (!picked) {
      closeRun(i);
      runStart = i + 1;
      continue;
    }
    events.selections.push_back({composer.Syllables(node.start, node.length), node.text,
                                 i > 0 ? nodes[i - 1].text : std::wstring()});
  }
  closeRun(i);
  return events;
}

double LearnedBonus(const UserDictionary& user, const Composer& composer,
                    const std::vector<Node>& nodes, double pickBonus, double contextBonus) {
  if (user.selectionCount() == 0) return 0;
  double bonus = 0;
  for (size_t i = 0; i < nodes.size(); ++i) {
    const Node& node = nodes[i];
    if (!composer.IsReadingSpan(node.start, node.length)) continue;
    const std::vector<Syllable> reading = composer.Syllables(node.start, node.length);
    const double overall = user.SelectionWeight(reading, node.text);
    if (overall <= 0) continue;
    bonus += pickBonus * std::min(1.0, overall);
    if (i > 0) {
      const double after = user.SelectionWeight(reading, node.text, nodes[i - 1].text);
      bonus += contextBonus * std::min(1.0, after);
    }
  }
  return bonus;
}

}  // namespace llaime
