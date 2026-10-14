#pragma once
#include <gtest/gtest.h>

#include <initializer_list>
#include <vector>

#include "llaime/Lexicon.h"

// Shared fixture loading the lexicon built by tools/build_lexicon.py.
inline const llaime::Lexicon& TestLexicon() {
  static llaime::Lexicon lexicon;
  static bool opened = lexicon.Open(LLAIME_TEST_LEXICON);
  EXPECT_TRUE(opened) << "build data/build/lexicon.bin first (see README)";
  return lexicon;
}

inline std::vector<llaime::Syllable> Seq(std::initializer_list<const wchar_t*> texts) {
  std::vector<llaime::Syllable> out;
  for (const wchar_t* t : texts) out.push_back(*llaime::Syllable::Parse(t));
  return out;
}
