// Privacy mode: password fields and private browsing windows, decided solely by the field type
// the app reports. While active, the taskbar icon shows a shield and nothing typed may be
// learned or logged.
#include <InputScope.h>

#include <algorithm>
#include <cwchar>

#include "EditSession.h"
#include "TextService.h"

using Microsoft::WRL::ComPtr;

namespace llaime {
namespace {

// {1713DD5A-68E7-4A5B-9AF6-592A595C778D}, declared in InputScope.h without a definition.
constexpr GUID kPropInputScope = {
    0x1713dd5a, 0x68e7, 0x4a5b, {0x9a, 0xf6, 0x59, 0x2a, 0x59, 0x5c, 0x77, 0x8d}};

bool IsPrivateScope(InputScope scope) {
  switch (scope) {
    case IS_PASSWORD:
    case IS_NUMERIC_PASSWORD:
    case IS_NUMERIC_PIN:
    case IS_ALPHANUMERIC_PIN:
    case IS_ALPHANUMERIC_PIN_SET:
    case IS_PRIVATE:  // Chrome incognito, Edge InPrivate, Firefox private windows
      return true;
    default:
      return false;
  }
}

// Reads the input scopes the app reports for `range`. Apps backed by a text store (Firefox,
// most TSF-aware apps) expose them as an app property through RequestSupportedAttrs; others
// set them as a regular property. Try both.
bool ScopesArePrivate(ITfReadOnlyProperty* prop, TfEditCookie ec, ITfRange* range) {
  VARIANT var;
  VariantInit(&var);
  if (!prop || prop->GetValue(ec, range, &var) != S_OK || var.vt != VT_UNKNOWN || !var.punkVal) {
    VariantClear(&var);
    return false;
  }
  ComPtr<ITfInputScope> inputScope;
  bool isPrivate = false;
  if (SUCCEEDED(var.punkVal->QueryInterface(IID_PPV_ARGS(&inputScope)))) {
    InputScope* scopes = nullptr;
    UINT count = 0;
    if (SUCCEEDED(inputScope->GetInputScopes(&scopes, &count)) && scopes) {
      isPrivate = std::any_of(scopes, scopes + count, IsPrivateScope);
      CoTaskMemFree(scopes);
    }
  }
  VariantClear(&var);
  return isPrivate;
}

bool InputScopesArePrivate(ITfContext* context, TfEditCookie ec) {
  TF_SELECTION sel{};
  ULONG fetched = 0;
  if (FAILED(context->GetSelection(ec, TF_DEFAULT_SELECTION, 1, &sel, &fetched)) || !fetched) {
    return false;
  }
  ComPtr<ITfRange> range;
  range.Attach(sel.range);

  ComPtr<ITfReadOnlyProperty> appProp;
  if (SUCCEEDED(context->GetAppProperty(kPropInputScope, &appProp)) &&
      ScopesArePrivate(appProp.Get(), ec, range.Get())) {
    return true;
  }
  ComPtr<ITfProperty> prop;
  return SUCCEEDED(context->GetProperty(kPropInputScope, &prop)) &&
         ScopesArePrivate(prop.Get(), ec, range.Get());
}

// Win32 Edit controls declare password fields with the ES_PASSWORD style instead.
bool IsPasswordEdit(HWND hwnd) {
  if (!hwnd) return false;
  wchar_t cls[32] = {};
  GetClassNameW(hwnd, cls, 32);
  return _wcsicmp(cls, L"Edit") == 0 && (GetWindowLongW(hwnd, GWL_STYLE) & ES_PASSWORD);
}

}  // namespace

void TextService::UpdatePrivacy(ITfContext* context, TfEditCookie ec) {
  bool isPrivate = InputScopesArePrivate(context, ec);
  if (!isPrivate) {
    ComPtr<ITfContextView> view;
    HWND hwnd = nullptr;
    if (SUCCEEDED(context->GetActiveView(&view))) view->GetWnd(&hwnd);
    isPrivate = IsPasswordEdit(hwnd ? hwnd : GetFocus());
  }
  SetPrivateMode(isPrivate);
}

void TextService::RequestPrivacyUpdate(ITfDocumentMgr* docMgr) {
  ComPtr<ITfContext> context;
  if (!docMgr || FAILED(docMgr->GetTop(&context)) || !context) {
    SetPrivateMode(false);
    return;
  }
  EditSession::Request(context.Get(), clientId_, TF_ES_ASYNCDONTCARE | TF_ES_READ,
                       [this, context](TfEditCookie ec) {
                         UpdatePrivacy(context.Get(), ec);
                         return S_OK;
                       });
}

}  // namespace llaime
