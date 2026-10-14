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

// Candidate window modeled on the Windows 11 Microsoft Bopomofo one. Two layouts:
//  - list: one numbered column of 9, page dots on the right, ▲▼ paging in the footer;
//  - expanded: 4 columns of 9 filled column by column (only the first is numbered), page dots
//    centered in the footer with ◀▶ paging.
// The footer also has the expand/collapse button and the emoji button. Never takes focus.
class CandidateWindow {
 public:
  struct Handlers {
    std::function<void(size_t index)> select;
    std::function<void()> emoji;  // footer emoji button
  };

  explicit CandidateWindow(Handlers handlers);
  ~CandidateWindow();
  CandidateWindow(const CandidateWindow&) = delete;
  CandidateWindow& operator=(const CandidateWindow&) = delete;

  static constexpr size_t kRows = 9;
  static constexpr size_t kExpandedColumns = 4;

  // Called before the DLL unloads; the class would otherwise outlive its window procedure.
  static void UnregisterClass();

  // `anchor` is the screen rectangle of the text the candidates apply to (physical pixels).
  void Show(std::vector<std::wstring> items, const RECT& anchor);
  void Hide();
  bool IsVisible() const { return hwnd_ && IsWindowVisible(hwnd_); }

  size_t Selection() const { return selection_; }
  size_t Count() const { return items_.size(); }
  bool Expanded() const { return expanded_; }
  void SetExpanded(bool expanded);

  // Up/Down: previous/next candidate. Left/Right: previous/next column when expanded, page in
  // the list layout.
  void MoveSelection(int delta);
  void MoveColumn(int delta);
  void MovePage(int delta);
  // Global index for the digit key `digit` (1..9): a row of the first column on this page.
  std::optional<size_t> IndexForDigit(int digit) const;

 private:
  enum class FooterButton { None, Previous, Next, Expand, Emoji };

  static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
  LRESULT HandleMessage(UINT msg, WPARAM wp, LPARAM lp);

  bool EnsureWindow();
  bool EnsureDeviceResources();
  void DiscardDeviceResources();
  void UpdateLayout(const RECT& anchor);
  void Paint();
  void PaintFooter(ID2D1SolidColorBrush* brush, float width, float height);
  float Scale() const { return dpi_ / 96.0f; }
  size_t Columns() const { return expanded_ ? kExpandedColumns : 1; }
  size_t PageSize() const { return kRows * Columns(); }
  size_t PageStart() const { return selection_ / PageSize() * PageSize(); }
  size_t PageCount() const { return (items_.size() + PageSize() - 1) / PageSize(); }
  // Cell rectangle (DIPs) of the item at `offset` within the current page.
  D2D1_RECT_F CellRect(size_t offset) const;
  int HitTestItem(POINT pt) const;  // global index, or -1
  FooterButton HitTestFooter(POINT pt) const;
  D2D1_RECT_F FooterButtonRect(FooterButton button) const;

  Handlers handlers_;
  HWND hwnd_ = nullptr;
  UINT dpi_ = 96;
  Theme theme_;
  std::vector<std::wstring> items_;
  size_t selection_ = 0;
  bool expanded_ = false;
  int hoverItem_ = -1;
  FooterButton hoverButton_ = FooterButton::None;
  float firstColumnWidth_ = 0;  // DIPs; includes the number column
  float columnWidth_ = 0;       // DIPs; columns 2..4 when expanded
  float widthDip_ = 0;
  float heightDip_ = 0;
  RECT anchor_{};

  Microsoft::WRL::ComPtr<ID2D1HwndRenderTarget> target_;
  Microsoft::WRL::ComPtr<IDWriteTextFormat> textFormat_;
  Microsoft::WRL::ComPtr<IDWriteTextFormat> numberFormat_;
  Microsoft::WRL::ComPtr<IDWriteTextFormat> iconFormat_;
  UINT formatDpi_ = 0;
};

}  // namespace llaime
