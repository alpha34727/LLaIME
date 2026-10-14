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
# The log-sum-exp over the vocabulary is the hot loop; let it vectorize.
target_compile_options(LLaIME.Llm PRIVATE /fp:fast)

# ONNX Runtime (CPU) for the Laya ranker, fetched by scripts/fetch-onnxruntime.ps1.
set(LLAIME_ORT_DIR "${CMAKE_SOURCE_DIR}/build/onnxruntime/onnxruntime-win-x64-1.30.0" CACHE PATH
    "ONNX Runtime release directory")
if(NOT EXISTS "${LLAIME_ORT_DIR}/lib/onnxruntime.lib")
  message(FATAL_ERROR "ONNX Runtime not found in ${LLAIME_ORT_DIR} (run scripts/fetch-onnxruntime.ps1)")
endif()
add_library(onnxruntime SHARED IMPORTED)
set_target_properties(onnxruntime PROPERTIES
  IMPORTED_IMPLIB "${LLAIME_ORT_DIR}/lib/onnxruntime.lib"
  IMPORTED_LOCATION "${LLAIME_ORT_DIR}/lib/onnxruntime.dll"
  INTERFACE_INCLUDE_DIRECTORIES "${LLAIME_ORT_DIR}/include")
set(LLAIME_ORT_DLLS "${LLAIME_ORT_DIR}/lib/onnxruntime.dll"
    "${LLAIME_ORT_DIR}/lib/onnxruntime_providers_shared.dll")

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
target_link_libraries(LLaIME.Engine PRIVATE LLaIME.Llm LLaIME.Laya LLaIME.Common)
add_custom_command(TARGET LLaIME.Engine POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${LLAIME_LLAMA_DLLS} ${LLAIME_ORT_DLLS}
          $<TARGET_FILE_DIR:LLaIME.Engine>
  COMMAND_EXPAND_LISTS)

add_executable(LLaIME.PipeProbe tools/PipeProbe.cpp)
target_link_libraries(LLaIME.PipeProbe PRIVATE LLaIME.Common)
