#pragma once
#include <d2d1.h>
#include <dwrite.h>
#include <windows.h>
#include <wrl/client.h>

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "ui/Theme.h"

namespace llaime {

// Vertical candidate list modeled on the Windows 11 Microsoft Bopomofo candidate window:
// numbered rows, a rounded highlight with an accent pill on the selected row, page dots on the
// right and paging arrows in a footer. Never takes focus.
class CandidateWindow {
 public:
  using SelectHandler = std::function<void(size_t index)>;

  explicit CandidateWindow(SelectHandler onSelect);
  ~CandidateWindow();
  CandidateWindow(const CandidateWindow&) = delete;
  CandidateWindow& operator=(const CandidateWindow&) = delete;

  static constexpr size_t kPageSize = 9;

  // Called before the DLL unloads; the class would otherwise outlive its window procedure.
  static void UnregisterClass();

  // `anchor` is the screen rectangle of the text the candidates apply to (physical pixels).
  void Show(std::vector<std::wstring> items, const RECT& anchor);
  void Hide();
  bool IsVisible() const { return hwnd_ && IsWindowVisible(hwnd_); }

  size_t Selection() const { return selection_; }
  size_t Count() const { return items_.size(); }
  void MoveSelection(int delta);
  void MovePage(int delta);
  // Global index for the digit key `digit` (1..9) on the current page.
  std::optional<size_t> IndexForDigit(int digit) const;

 private:
  static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
  LRESULT HandleMessage(UINT msg, WPARAM wp, LPARAM lp);

  bool EnsureWindow();
  bool EnsureDeviceResources();
  void DiscardDeviceResources();
  void UpdateLayout(const RECT& anchor);
  void Paint();
  float Scale() const { return dpi_ / 96.0f; }
  size_t PageStart() const { return selection_ / kPageSize * kPageSize; }
  size_t PageCount() const { return (items_.size() + kPageSize - 1) / kPageSize; }
  int HitTestRow(POINT pt) const;  // -1 if none
  int HitTestFooter(POINT pt) const;  // -1 page up, +1 page down, 0 none

  SelectHandler onSelect_;
  HWND hwnd_ = nullptr;
  UINT dpi_ = 96;
  Theme theme_;
  std::vector<std::wstring> items_;
  size_t selection_ = 0;
  int hoverRow_ = -1;
  float widthDip_ = 0;
  RECT anchor_{};

  Microsoft::WRL::ComPtr<ID2D1HwndRenderTarget> target_;
  Microsoft::WRL::ComPtr<IDWriteTextFormat> textFormat_;
  Microsoft::WRL::ComPtr<IDWriteTextFormat> numberFormat_;
  UINT formatDpi_ = 0;
};

}  // namespace llaime
