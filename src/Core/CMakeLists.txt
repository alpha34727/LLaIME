add_library(LLaIME.Core STATIC
  Syllable.cpp
  BopomofoReading.cpp
  Lexicon.cpp
  Composer.cpp
  UserDictionary.cpp
  Learning.cpp
)
target_include_directories(LLaIME.Core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(LLaIME.Core PUBLIC LLaIME.Common)
