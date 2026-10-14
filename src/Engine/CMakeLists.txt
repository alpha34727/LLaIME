# llama.cpp is built separately (scripts/build-llama.ps1) with dynamically loaded backends.
set(LLAIME_LLAMA_DIR "${CMAKE_SOURCE_DIR}/build/llama/Release/install" CACHE PATH
    "llama.cpp install prefix")
find_package(llama CONFIG PATHS "${LLAIME_LLAMA_DIR}" NO_DEFAULT_PATH)
if(NOT llama_FOUND)
  message(STATUS "llama.cpp not found in ${LLAIME_LLAMA_DIR}; skipping the engine (run scripts/build-llama.ps1)")
  return()
endif()

add_library(LLaIME.Llm STATIC LlmScorer.cpp)
target_include_directories(LLaIME.Llm PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(LLaIME.Llm PUBLIC llama)

add_executable(LLaIME.Eval tools/Eval.cpp)
target_link_libraries(LLaIME.Eval PRIVATE LLaIME.Llm LLaIME.Core)

# Ship llama/ggml DLLs (including the backend DLLs loaded at runtime) next to the binaries.
file(GLOB LLAIME_LLAMA_DLLS "${LLAIME_LLAMA_DIR}/bin/*.dll")
add_custom_command(TARGET LLaIME.Eval POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${LLAIME_LLAMA_DLLS} $<TARGET_FILE_DIR:LLaIME.Eval>
  COMMAND_EXPAND_LISTS)
