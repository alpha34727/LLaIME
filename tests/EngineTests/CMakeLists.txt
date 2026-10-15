include(GoogleTest)
add_executable(LLaIME.EngineTests UserStoreTests.cpp)
target_link_libraries(LLaIME.EngineTests PRIVATE LLaIME.UserStore GTest::gtest_main)
gtest_discover_tests(LLaIME.EngineTests DISCOVERY_MODE PRE_TEST)
