# Flat C API over the IME's own code for the WinUI 3 settings app (src/Settings/App).
add_library(LLaIME.SettingsNative SHARED SettingsBridge.cpp)
target_link_libraries(LLaIME.SettingsNative PRIVATE LLaIME.UserStore LLaIME.Core ole32)
set_target_properties(LLaIME.SettingsNative PROPERTIES OUTPUT_NAME "LLaIME.SettingsNative")
