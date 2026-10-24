# The user dictionary store (SQLite amalgamation, public domain) does not need the model
# runtimes, so it and its tests build first.
include(FetchContent)
FetchContent_Declare(
  sqlite
  URL https://sqlite.org/2026/sqlite-amalgamation-3540000.zip
  URL_HASH SHA3_256=7b670a62fdfbd672b75fef004cb703c8a3e87d3a5cc7d675b4a08337004a2d93
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(sqlite)
add_library(sqlite3 STATIC ${sqlite_SOURCE_DIR}/sqlite3.c)
target_include_directories(sqlite3 PUBLIC ${sqlite_SOURCE_DIR})
target_compile_definitions(sqlite3 PRIVATE SQLITE_ENABLE_MATH_FUNCTIONS SQLITE_DQS=0
  SQLITE_DEFAULT_FOREIGN_KEYS=1 SQLITE_OMIT_LOAD_EXTENSION SQLITE_THREADSAFE=1)
target_compile_options(sqlite3 PRIVATE /W0)

add_library(LLaIME.UserStore STATIC UserStore.cpp)
target_include_directories(LLaIME.UserStore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(LLaIME.UserStore PUBLIC LLaIME.Core LLaIME.Common PRIVATE sqlite3)

# llama.cpp is built separately (scripts/build-llama.ps1) with dynamically loaded backends.
set(LLAIME_LLAMA_DIR "${CMAKE_SOURCE_DIR}/build/llama/Release/install" CACHE PATH
    "llama.cpp install prefix")
find_package(llama CONFIG PATHS "${LLAIME_LLAMA_DIR}" NO_DEFAULT_PATH)
if(NOT llama_FOUND)
  message(STATUS "llama.cpp not found in ${LLAIME_LLAMA_DIR}; skipping the engine (run scripts/build-llama.ps1)")
  return()
endif()

add_library(LLaIME.Llm STATIC LlmScorer.cpp LlmDecoder.cpp)
target_include_directories(LLaIME.Llm PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(LLaIME.Llm PUBLIC llama)
# The log-sum-exp over the vocabulary is the hot loop; let it vectorize.
target_compile_options(LLaIME.Llm PRIVATE /fp:fast)

# ONNX Runtime (CPU, and CUDA for NVIDIA GPUs) for the Laya ranker, fetched by
# scripts/fetch-onnxruntime.ps1.
set(LLAIME_ORT_DIR "${CMAKE_SOURCE_DIR}/build/onnxruntime/onnxruntime-win-x64-gpu_cuda13-1.30.0"
    CACHE PATH "ONNX Runtime release directory")
if(NOT EXISTS "${LLAIME_ORT_DIR}/lib/onnxruntime.lib")
  message(FATAL_ERROR "ONNX Runtime not found in ${LLAIME_ORT_DIR} (run scripts/fetch-onnxruntime.ps1)")
endif()
add_library(onnxruntime SHARED IMPORTED)
set_target_properties(onnxruntime PROPERTIES
  IMPORTED_IMPLIB "${LLAIME_ORT_DIR}/lib/onnxruntime.lib"
  IMPORTED_LOCATION "${LLAIME_ORT_DIR}/lib/onnxruntime.dll"
  INTERFACE_INCLUDE_DIRECTORIES "${LLAIME_ORT_DIR}/include")
set(LLAIME_ORT_DLLS "${LLAIME_ORT_DIR}/lib/onnxruntime.dll"
    "${LLAIME_ORT_DIR}/lib/onnxruntime_providers_shared.dll"
    "${LLAIME_ORT_DIR}/lib/onnxruntime_providers_cuda.dll")

add_library(LLaIME.Laya STATIC LayaTokenizer.cpp LayaScorer.cpp)
target_include_directories(LLaIME.Laya PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(LLaIME.Laya PUBLIC onnxruntime)

add_executable(LLaIME.Eval tools/Eval.cpp)
target_link_libraries(LLaIME.Eval PRIVATE LLaIME.Llm LLaIME.Laya LLaIME.Core)

# Ship llama/ggml DLLs (including the backend DLLs loaded at runtime) next to the binaries.
file(GLOB LLAIME_LLAMA_DLLS "${LLAIME_LLAMA_DIR}/bin/*.dll")
# The engine's own onnxruntime.dll must sit next to it: Windows also ships an older one in System32.
add_custom_command(TARGET LLaIME.Eval POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${LLAIME_LLAMA_DLLS} ${LLAIME_ORT_DLLS}
          $<TARGET_FILE_DIR:LLaIME.Eval>
  COMMAND_EXPAND_LISTS)

add_executable(LLaIME.Engine EngineMain.cpp)
target_link_libraries(LLaIME.Engine PRIVATE LLaIME.Llm LLaIME.Laya LLaIME.UserStore LLaIME.Common)
add_custom_command(TARGET LLaIME.Engine POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${LLAIME_LLAMA_DLLS} ${LLAIME_ORT_DLLS}
          $<TARGET_FILE_DIR:LLaIME.Engine>
  COMMAND_EXPAND_LISTS)

add_executable(LLaIME.PipeProbe tools/PipeProbe.cpp)
target_link_libraries(LLaIME.PipeProbe PRIVATE LLaIME.Core LLaIME.Common)

# Simulates a user correcting conversions, to measure what learning gains and costs.
add_executable(LLaIME.LearnSim tools/LearnSim.cpp)
target_link_libraries(LLaIME.LearnSim PRIVATE LLaIME.Llm LLaIME.UserStore LLaIME.Core)
add_custom_command(TARGET LLaIME.LearnSim POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${LLAIME_LLAMA_DLLS}
          $<TARGET_FILE_DIR:LLaIME.LearnSim>
  COMMAND_EXPAND_LISTS)

# Types through the installed IME into normal and private fields and checks what is learned.
add_executable(LLaIME.PrivacyTest tools/PrivacyTest.cpp)
target_link_libraries(LLaIME.PrivacyTest PRIVATE LLaIME.UserStore LLaIME.Common)
# End-to-end test of turning a lexicon layer off in the installed IME.
add_executable(LLaIME.LexiconTest tools/LexiconTest.cpp)
target_link_libraries(LLaIME.LexiconTest PRIVATE LLaIME.Core)
# End-to-end test of user phrase shortcuts (` p, then the keys) in the installed IME.
add_executable(LLaIME.ShortcutTest tools/ShortcutTest.cpp)
target_link_libraries(LLaIME.ShortcutTest PRIVATE LLaIME.UserStore)
# Types into another program (e.g. Word) with saves mid-composition; checks it survives.
add_executable(LLaIME.TypeTest tools/TypeTest.cpp)
