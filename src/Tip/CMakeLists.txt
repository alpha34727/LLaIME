add_library(LLaIME.Tip SHARED
  DllMain.cpp
  Globals.cpp
  ClassFactory.cpp
  Register.cpp
  TextService.cpp
  KeyEventSink.cpp
  KeyMapping.cpp
  InputMode.cpp
  Composition.cpp
  DisplayAttribute.cpp
  ui/Theme.cpp
  ui/CandidateWindow.cpp
  Tip.def
  Tip.rc
)
target_link_libraries(LLaIME.Tip PRIVATE LLaIME.Core d2d1 dwrite dwmapi shcore)
target_include_directories(LLaIME.Tip PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(LLaIME.Tip PROPERTIES OUTPUT_NAME "LLaIME.Tip")
