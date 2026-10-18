#pragma once
// The IME runs inside other programs. A C++ exception that leaves it through a COM method,
// window procedure or thread ends the host program (0xE06D7363), so every entry point runs its
// body through Guarded(): an exception is caught, written to
// %LOCALAPPDATA%\LLaIME\tip-errors.log (in a packaged app, possibly that app's redirected
// copy of the folder) and turned into a failure result.
#include <exception>
#include <type_traits>
#include <utility>

namespace llaime {

void ReportException(const char* where, const char* what) noexcept;

template <typename R, typename F>
R Guarded(const char* where, R fallback, F&& f) noexcept {
  try {
    return std::forward<F>(f)();
  } catch (const std::exception& e) {
    ReportException(where, e.what());
  } catch (...) {
    ReportException(where, "non-standard exception");
  }
  return fallback;
}

template <typename F>
void Guarded(const char* where, F&& f) noexcept {
  try {
    std::forward<F>(f)();
  } catch (const std::exception& e) {
    ReportException(where, e.what());
  } catch (...) {
    ReportException(where, "non-standard exception");
  }
}

}  // namespace llaime
