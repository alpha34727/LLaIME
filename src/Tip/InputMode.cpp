// Chinese/English mode, kept in the keyboard compartments so the taskbar input indicator
// shows 中/英 and clicking it switches modes.
#include "EditSession.h"
#include "LangBarButton.h"
#include "TextService.h"
#include "ui/ThemeWatcher.h"

using Microsoft::WRL::ComPtr;

namespace llaime {
namespace {

ComPtr<ITfCompartment> GetCompartment(ITfThreadMgr* threadMgr, REFGUID guid) {
  ComPtr<ITfCompartmentMgr> mgr;
  ComPtr<ITfCompartment> compartment;
  if (threadMgr && SUCCEEDED(threadMgr->QueryInterface(IID_PPV_ARGS(&mgr)))) {
    mgr->GetCompartment(guid, &compartment);
  }
  return compartment;
}

bool ReadDword(ITfCompartment* compartment, DWORD* value) {
  VARIANT var;
  VariantInit(&var);
  if (!compartment || compartment->GetValue(&var) != S_OK || var.vt != VT_I4) return false;
  *value = static_cast<DWORD>(var.lVal);
  return true;
}

void WriteDword(ITfCompartment* compartment, TfClientId clientId, DWORD value) {
  if (!compartment) return;
  VARIANT var;
  VariantInit(&var);
  var.vt = VT_I4;
  var.lVal = static_cast<LONG>(value);
  compartment->SetValue(clientId, &var);
}

bool Advise(ITfCompartment* compartment, ITfCompartmentEventSink* sink, DWORD* cookie) {
  ComPtr<ITfSource> source;
  return compartment && SUCCEEDED(compartment->QueryInterface(IID_PPV_ARGS(&source))) &&
         SUCCEEDED(source->AdviseSink(IID_ITfCompartmentEventSink, sink, cookie));
}

void Unadvise(ITfCompartment* compartment, DWORD* cookie) {
  ComPtr<ITfSource> source;
  if (*cookie != TF_INVALID_COOKIE && compartment &&
      SUCCEEDED(compartment->QueryInterface(IID_PPV_ARGS(&source)))) {
    source->UnadviseSink(*cookie);
  }
  *cookie = TF_INVALID_COOKIE;
}

}  // namespace

bool TextService::InitInputMode() {
  auto openClose = GetCompartment(threadMgr_.Get(), GUID_COMPARTMENT_KEYBOARD_OPENCLOSE);
  auto conversion =
      GetCompartment(threadMgr_.Get(), GUID_COMPARTMENT_KEYBOARD_INPUTMODE_CONVERSION);
  auto* sink = static_cast<ITfCompartmentEventSink*>(this);
  Advise(openClose.Get(), sink, &openCloseSinkCookie_);
  Advise(conversion.Get(), sink, &conversionSinkCookie_);
  SetChineseMode(true);
  return true;
}

void TextService::UninitInputMode() {
  Unadvise(GetCompartment(threadMgr_.Get(), GUID_COMPARTMENT_KEYBOARD_OPENCLOSE).Get(),
           &openCloseSinkCookie_);
  Unadvise(GetCompartment(threadMgr_.Get(), GUID_COMPARTMENT_KEYBOARD_INPUTMODE_CONVERSION).Get(),
           &conversionSinkCookie_);
}

void TextService::SetChineseMode(bool chinese) {
  chineseMode_ = chinese;
  if (!chinese) CloseCandidates();
  WriteConversionMode();
}

void TextService::SetFullShape(bool full) {
  fullShape_ = full;
  WriteConversionMode();
}

void TextService::WriteConversionMode() {
  DWORD mode = chineseMode_ ? TF_CONVERSIONMODE_NATIVE : TF_CONVERSIONMODE_ALPHANUMERIC;
  if (fullShape_) mode |= TF_CONVERSIONMODE_FULLSHAPE;
  updatingCompartment_ = true;
  WriteDword(GetCompartment(threadMgr_.Get(), GUID_COMPARTMENT_KEYBOARD_OPENCLOSE).Get(),
             clientId_, TRUE);
  WriteDword(GetCompartment(threadMgr_.Get(), GUID_COMPARTMENT_KEYBOARD_INPUTMODE_CONVERSION).Get(),
             clientId_, mode);
  updatingCompartment_ = false;
  if (langBarButton_) langBarButton_->Refresh();
}

void TextService::SetPrivateMode(bool isPrivate) {
  if (privateMode_ == isPrivate) return;
  privateMode_ = isPrivate;
  if (langBarButton_) langBarButton_->Refresh();
}

void TextService::ToggleChineseFromUi() {
  if (chineseMode_ && IsComposing() && compositionContext_) {
    ComPtr<ITfContext> context = compositionContext_;
    EditSession::Request(context.Get(), clientId_, TF_ES_SYNC | TF_ES_READWRITE,
                         [this, context](TfEditCookie ec) {
                           CommitAll(context.Get(), ec);
                           return S_OK;
                         });
  }
  SetChineseMode(!chineseMode_);
}

bool TextService::InitLangBar() {
  ComPtr<ITfLangBarItemMgr> mgr;
  if (FAILED(threadMgr_.As(&mgr))) return false;
  langBarButton_ = new LangBarButton({
      [this] { return chineseMode_; },
      [this] { return fullShape_; },
      [this] { return privateMode_; },
      [this] { ToggleChineseFromUi(); },
      [this] { SetFullShape(!fullShape_); },
      [this] { return !learningEnabled_; },
      [this] { SetLearningEnabled(!learningEnabled_); },
  });
  if (FAILED(mgr->AddItem(langBarButton_))) {
    langBarButton_->Release();
    langBarButton_ = nullptr;
    return false;
  }
  themeWatcher_ = std::make_unique<ThemeWatcher>([this] {
    if (langBarButton_) langBarButton_->Refresh();
  });
  return true;
}

void TextService::UninitLangBar() {
  themeWatcher_.reset();
  if (!langBarButton_) return;
  ComPtr<ITfLangBarItemMgr> mgr;
  if (threadMgr_ && SUCCEEDED(threadMgr_.As(&mgr))) mgr->RemoveItem(langBarButton_);
  langBarButton_->Release();
  langBarButton_ = nullptr;
}

STDMETHODIMP TextService::OnChange(REFGUID guid) {
  if (updatingCompartment_) return S_OK;
  DWORD value = 0;
  if (guid == GUID_COMPARTMENT_KEYBOARD_INPUTMODE_CONVERSION) {
    if (ReadDword(GetCompartment(threadMgr_.Get(), guid).Get(), &value)) {
      chineseMode_ = (value & TF_CONVERSIONMODE_NATIVE) != 0;
      fullShape_ = (value & TF_CONVERSIONMODE_FULLSHAPE) != 0;
    }
  } else if (guid == GUID_COMPARTMENT_KEYBOARD_OPENCLOSE) {
    if (ReadDword(GetCompartment(threadMgr_.Get(), guid).Get(), &value) && !value) {
      chineseMode_ = false;
    }
  }
  if (!chineseMode_) CloseCandidates();
  if (langBarButton_) langBarButton_->Refresh();
  return S_OK;
}

}  // namespace llaime
