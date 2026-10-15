include(FetchContent)
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/releases/download/v1.17.0/googletest-1.17.0.tar.gz
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
set(gtest_force_shared_crt OFF CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

add_executable(LLaIME.CoreTests
  SyllableTests.cpp
  ReadingTests.cpp
  LexiconTests.cpp
  ComposerTests.cpp
  UserDictionaryTests.cpp
  LexiconBuilderTests.cpp
)
target_link_libraries(LLaIME.CoreTests PRIVATE LLaIME.Core GTest::gtest_main)
target_compile_definitions(LLaIME.CoreTests PRIVATE
  LLAIME_TEST_LEXICON=L"${CMAKE_SOURCE_DIR}/data/build/lexicons")

include(GoogleTest)
gtest_discover_tests(LLaIME.CoreTests DISCOVERY_MODE PRE_TEST)
