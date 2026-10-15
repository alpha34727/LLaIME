#pragma once
#include <ctfutb.h>
#include <msctf.h>
#include <wrl/client.h>

#include <functional>

namespace llaime {

// {2C77A81E-41CC-4178-A3A7-5F8A987568E6}: the taskbar input indicator item. Windows 8+ shows
// only this language bar item, next to the profile icon.
inline constexpr GUID GUID_LBI_InputMode = {
    0x2c77a81e, 0x41cc, 0x4178, {0xa3, 0xa7, 0x5f, 0x8a, 0x98, 0x75, 0x68, 0xe6}};

// The 中/英 button in the taskbar. Left click toggles Chinese/English; right click shows a menu.
class LangBarButton final : public ITfLangBarItemButton, public ITfSource {
 public:
  struct Callbacks {
    std::function<bool()> isChinese;
    std::function<bool()> isFullShape;
    std::function<bool()> isPrivate;
    std::function<void()> toggleChinese;
    std::function<void()> toggleFullShape;
    std::function<bool()> isLearningPaused;
    std::function<void()> toggleLearning;
    std::function<void()> openSettings;
  };

  explicit LangBarButton(Callbacks callbacks);

  // Tells Windows to re-query the icon and tooltip after a mode change.
  void Refresh();

  // IUnknown
  STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
  STDMETHODIMP_(ULONG) AddRef() override;
  STDMETHODIMP_(ULONG) Release() override;

  // ITfLangBarItem
  STDMETHODIMP GetInfo(TF_LANGBARITEMINFO* info) override;
  STDMETHODIMP GetStatus(DWORD* status) override;
  STDMETHODIMP Show(BOOL show) override;
  STDMETHODIMP GetTooltipString(BSTR* tooltip) override;

  // ITfLangBarItemButton
  STDMETHODIMP OnClick(TfLBIClick click, POINT pt, const RECT* area) override;
  STDMETHODIMP InitMenu(ITfMenu* menu) override;
  STDMETHODIMP OnMenuSelect(UINT id) override;
  STDMETHODIMP GetIcon(HICON* icon) override;
  STDMETHODIMP GetText(BSTR* text) override;

  // ITfSource
  STDMETHODIMP AdviseSink(REFIID riid, IUnknown* sink, DWORD* cookie) override;
  STDMETHODIMP UnadviseSink(DWORD cookie) override;

 private:
  ~LangBarButton();

  LONG ref_ = 1;
  Callbacks callbacks_;
  Microsoft::WRL::ComPtr<ITfLangBarItemSink> sink_;
};

}  // namespace llaime
