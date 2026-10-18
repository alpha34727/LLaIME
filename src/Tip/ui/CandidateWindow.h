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
// Each candidate can carry the language model's probability that it is the intended one: a bar
// under the text, and the percentage beside the selected candidate.
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
  // `withProbabilities` reserves room for the percentages, which arrive later.
  void Show(std::vector<std::wstring> items, const RECT& anchor, bool withProbabilities = false);
  // One value in [0, 1] per item (or fewer: the rest have none); negative means unknown.
  // A non-empty `order` also reorders the list: position i shows the item that was at
  // order[i], and `probabilities` are in the new order. A selection the user moved stays on
  // its item; the default first-row selection stays on the first row.
  void SetProbabilities(std::vector<double> probabilities, const std::vector<size_t>& order = {});
  void Hide();
  bool IsVisible() const { return uiless_ ? !items_.empty() : hwnd_ && IsWindowVisible(hwnd_); }
  // UI-less: the app draws the candidates itself (Windows Search, Sticky Notes), so this only
  // keeps the list and the selection. Set before Show.
  void SetUiLess(bool uiless) { uiless_ = uiless; }
  bool UiLess() const { return uiless_; }
  const std::wstring& Item(size_t index) const { return items_[index]; }

  size_t Selection() const { return selection_; }
  size_t Count() const { return items_.size(); }
  bool Expanded() const { return expanded_; }
  void SetExpanded(bool expanded);

  // Up/Down: previous/next candidate. Right expands the list, then moves column by column,
  // on to the next page after the last column, and from the last page back to the first; Left
  // moves back a column, to the previous page's last column, and collapses from the first
  // column of the first page (in the list layout it pages back).
  void MoveSelection(int delta);
  void MoveColumn(int delta);
  void MovePage(int delta);
  // Global index for the digit key `digit` (1..9): a row of the first column on this page.
  std::optional<size_t> IndexForDigit(int digit) const;

 private:
  enum class FooterButton { None, Previous, Next, Expand, Emoji };

  static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
  LRESULT HandleMessage(UINT msg, WPARAM wp, LPARAM lp);

  void Invalidate();
  bool EnsureWindow();
  bool EnsureDeviceResources();
  void DiscardDeviceResources();
  void UpdateLayout(const RECT& anchor);
  void Paint();
  void PaintFooter(ID2D1SolidColorBrush* brush, float width, float height);
  void PaintProbability(ID2D1SolidColorBrush* brush, size_t index, float textX, float textRight,
                        const D2D1_RECT_F& cell);
  double ProbabilityOf(size_t index) const {
    return index < probabilities_.size() ? probabilities_[index] : -1;
  }
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
  std::vector<double> probabilities_;
  bool withProbabilities_ = false;
  size_t selection_ = 0;
  bool expanded_ = false;
  bool uiless_ = false;
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
  Microsoft::WRL::ComPtr<IDWriteTextFormat> percentFormat_;
  UINT formatDpi_ = 0;
};

}  // namespace llaime
