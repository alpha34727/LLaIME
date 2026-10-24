cmake_minimum_required(VERSION 3.28)
project(LLaIME VERSION 0.3.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_compile_definitions(UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
add_compile_options(/utf-8 /W4 /permissive- /Zc:__cplusplus)

option(LLAIME_BUILD_ENGINE "Build the engine (x64 only)" ON)
option(LLAIME_BUILD_TESTS "Build tests" ON)

add_subdirectory(src/Common)
add_subdirectory(src/Core)
add_subdirectory(src/Tip)

if(LLAIME_BUILD_ENGINE AND CMAKE_SIZEOF_VOID_P EQUAL 8 AND EXISTS ${CMAKE_SOURCE_DIR}/src/Engine/CMakeLists.txt)
  add_subdirectory(src/Engine)
endif()

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
  add_subdirectory(src/Setup)
endif()

# The settings app's native bridge needs the user dictionary store (src/Engine).
if(TARGET LLaIME.UserStore)
  add_subdirectory(src/Settings/Native)
endif()

if(LLAIME_BUILD_TESTS AND CMAKE_SIZEOF_VOID_P EQUAL 8)
  enable_testing()
  add_subdirectory(tests/CoreTests)
  if(TARGET LLaIME.UserStore)
    add_subdirectory(tests/EngineTests)
  endif()
endif()
