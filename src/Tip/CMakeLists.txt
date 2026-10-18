# UI pieces are a static library so the preview tool can render them outside a host app.
add_library(LLaIME.TipUi STATIC
  Globals.cpp
  Guard.cpp
  ui/Theme.cpp
  ui/Graphics.cpp
  ui/TextIcon.cpp
  ui/PopupMenu.cpp
  ui/ThemeWatcher.cpp
  ui/CandidateWindow.cpp
)
target_include_directories(LLaIME.TipUi PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(LLaIME.TipUi PUBLIC d2d1 dwrite dwmapi shcore windowscodecs)

add_library(LLaIME.Tip SHARED
  DllMain.cpp
  ClassFactory.cpp
  Register.cpp
  TextService.cpp
  KeyEventSink.cpp
  KeyMapping.cpp
  InputMode.cpp
  Composition.cpp
  DisplayAttribute.cpp
  LangBarButton.cpp
  Privacy.cpp
  EngineClient.cpp
  Ranking.cpp
  UserDict.cpp
  Lexicons.cpp
  Shortcut.cpp
  Tip.def
  Tip.rc
)
target_link_libraries(LLaIME.Tip PRIVATE LLaIME.Core LLaIME.TipUi)
set_target_properties(LLaIME.Tip PROPERTIES OUTPUT_NAME "LLaIME.Tip")

# Renders the IME's windows to PNG files for visual checks (dev only).
add_executable(LLaIME.UiPreview WIN32 tools/UiPreview.cpp)
target_link_libraries(LLaIME.UiPreview PRIVATE LLaIME.TipUi windowscodecs)
