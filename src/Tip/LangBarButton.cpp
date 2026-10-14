#include "LangBarButton.h"

#include <olectl.h>
#include <strsafe.h>

#include "Globals.h"
#include "ui/PopupMenu.h"
#include "ui/TextIcon.h"

namespace llaime {
namespace {

constexpr DWORD kSinkCookie = 0x4c4c4149;  // 'LLAI'

enum MenuId : UINT { kMenuChinese = 1, kMenuEnglish, kMenuHalfShape, kMenuFullShape };

}  // namespace

LangBarButton::LangBarButton(Callbacks callbacks) : callbacks_(std::move(callbacks)) {
  DllAddRef();
}

LangBarButton::~LangBarButton() { DllRelease(); }

STDMETHODIMP LangBarButton::QueryInterface(REFIID riid, void** ppv) {
  if (!ppv) return E_INVALIDARG;
  *ppv = nullptr;
  if (riid == IID_IUnknown || riid == IID_ITfLangBarItem || riid == IID_ITfLangBarItemButton) {
    *ppv = static_cast<ITfLangBarItemButton*>(this);
  } else if (riid == IID_ITfSource) {
    *ppv = static_cast<ITfSource*>(this);
  } else {
    return E_NOINTERFACE;
  }
  AddRef();
  return S_OK;
}

STDMETHODIMP_(ULONG) LangBarButton::AddRef() { return InterlockedIncrement(&ref_); }

STDMETHODIMP_(ULONG) LangBarButton::Release() {
  LONG r = InterlockedDecrement(&ref_);
  if (r == 0) delete this;
  return r;
}

void LangBarButton::Refresh() {
  if (sink_) sink_->OnUpdate(TF_LBI_ICON | TF_LBI_TEXT | TF_LBI_TOOLTIP);
}

STDMETHODIMP LangBarButton::GetInfo(TF_LANGBARITEMINFO* info) {
  if (!info) return E_INVALIDARG;
  *info = {};
  info->clsidService = CLSID_TextService;
  info->guidItem = GUID_LBI_InputMode;
  // The shell recolors mask-only icons to the taskbar text color with TEXTCOLORICON.
  // SHOWNINTRAY is documented as unsupported but is what keeps the item in the taskbar.
  info->dwStyle = TF_LBI_STYLE_BTN_BUTTON | TF_LBI_STYLE_SHOWNINTRAY | TF_LBI_STYLE_TEXTCOLORICON;
  info->ulSort = 0;
  StringCchCopyW(info->szDescription, ARRAYSIZE(info->szDescription), L"LLaIME 輸入模式");
  return S_OK;
}

STDMETHODIMP LangBarButton::GetStatus(DWORD* status) {
  if (!status) return E_INVALIDARG;
  *status = 0;
  return S_OK;
}

STDMETHODIMP LangBarButton::Show(BOOL) { return E_NOTIMPL; }

STDMETHODIMP LangBarButton::GetTooltipString(BSTR* tooltip) {
  if (!tooltip) return E_INVALIDARG;
  const bool chinese = callbacks_.isChinese();
  const bool full = callbacks_.isFullShape();
  wchar_t text[64];
  StringCchPrintfW(text, ARRAYSIZE(text), L"%s模式（%s）", chinese ? L"中文" : L"英數",
                   full ? L"全形" : L"半形");
  *tooltip = SysAllocString(text);
  return *tooltip ? S_OK : E_OUTOFMEMORY;
}

STDMETHODIMP LangBarButton::OnClick(TfLBIClick click, POINT, const RECT*) {
  if (click == TF_LBI_CLK_LEFT) {
    callbacks_.toggleChinese();
    return S_OK;
  }
  // Right click: a Windows 11 style menu like Microsoft Bopomofo's.
  const bool chinese = callbacks_.isChinese();
  const bool full = callbacks_.isFullShape();
  std::vector<PopupMenuItem> items = {
      {kMenuChinese, L"中文", chinese},
      {kMenuEnglish, L"英數", !chinese},
      {0, L"", false, true},
      {kMenuHalfShape, L"半形", !full},
      {kMenuFullShape, L"全形", full},
  };
  OnMenuSelect(ShowPopupMenu(items));
  return S_OK;
}

STDMETHODIMP LangBarButton::InitMenu(ITfMenu*) { return S_OK; }

STDMETHODIMP LangBarButton::OnMenuSelect(UINT id) {
  const bool chinese = callbacks_.isChinese();
  const bool full = callbacks_.isFullShape();
  switch (id) {
    case kMenuChinese: if (!chinese) callbacks_.toggleChinese(); break;
    case kMenuEnglish: if (chinese) callbacks_.toggleChinese(); break;
    case kMenuHalfShape: if (full) callbacks_.toggleFullShape(); break;
    case kMenuFullShape: if (!full) callbacks_.toggleFullShape(); break;
    default: break;
  }
  return S_OK;
}

STDMETHODIMP LangBarButton::GetIcon(HICON* icon) {
  if (!icon) return E_INVALIDARG;
  *icon = CreateMonochromeTextIcon(callbacks_.isChinese() ? L"中" : L"英", LangBarIconSize());
  return *icon ? S_OK : E_FAIL;
}

STDMETHODIMP LangBarButton::GetText(BSTR* text) {
  if (!text) return E_INVALIDARG;
  *text = SysAllocString(callbacks_.isChinese() ? L"中" : L"英");
  return *text ? S_OK : E_OUTOFMEMORY;
}

STDMETHODIMP LangBarButton::AdviseSink(REFIID riid, IUnknown* sink, DWORD* cookie) {
  if (!sink || !cookie) return E_INVALIDARG;
  if (riid != IID_ITfLangBarItemSink) return CONNECT_E_CANNOTCONNECT;
  if (sink_) return CONNECT_E_ADVISELIMIT;
  if (FAILED(sink->QueryInterface(IID_PPV_ARGS(&sink_)))) return E_NOINTERFACE;
  *cookie = kSinkCookie;
  return S_OK;
}

STDMETHODIMP LangBarButton::UnadviseSink(DWORD cookie) {
  if (cookie != kSinkCookie || !sink_) return CONNECT_E_NOCONNECTION;
  sink_.Reset();
  return S_OK;
}

}  // namespace llaime
