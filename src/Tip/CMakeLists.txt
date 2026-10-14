add_library(LLaIME.Tip SHARED
  DllMain.cpp
  Globals.cpp
  ClassFactory.cpp
  Register.cpp
  TextService.cpp
  KeyEventSink.cpp
  Tip.def
  Tip.rc
)
target_link_libraries(LLaIME.Tip PRIVATE LLaIME.Common)
target_include_directories(LLaIME.Tip PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(LLaIME.Tip PROPERTIES OUTPUT_NAME "LLaIME.Tip")
