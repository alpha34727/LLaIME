#include "TextService.h"


#include "DisplayAttribute.h"
#include "EditSession.h"
#include "Globals.h"
#include "Guard.h"
#include "ui/CandidateWindow.h"
#include "EngineClient.h"
#include "ui/ThemeWatcher.h"

using Microsoft::WRL::ComPtr;

namespace llaime {
namespace {

std::shared_ptr<const BopomofoLayout> StandardLayout() {
  static std::shared_ptr<const BopomofoLayout> layout = CreateStandardLayout();
  return layout;
}

}  // namespace

TextService::TextService()
    : lexiconRef_(CurrentLexicon()),
      lexicon_(lexiconRef_.get()),
      reading_(StandardLayout()),
      composer_(lexicon_) {
  DllAddRef();
}

TextService::~TextService() { DllRelease(); }

STDMETHODIMP TextService::QueryInterface(REFIID riid, void** ppv) {
  return Guarded("QueryInterface", HRESULT(E_FAIL), [&]() -> HRESULT {
    if (!ppv) return E_INVALIDARG;
    *ppv = nullptr;
    if (riid == IID_IUnknown || riid == IID_ITfTextInputProcessor ||
        riid == IID_ITfTextInputProcessorEx) {
      *ppv = static_cast<ITfTextInputProcessorEx*>(this);
    } else if (riid == IID_ITfThreadMgrEventSink) {
      *ppv = static_cast<ITfThreadMgrEventSink*>(this);
    } else if (riid == IID_ITfKeyEventSink) {
      *ppv = static_cast<ITfKeyEventSink*>(this);
    } else if (riid == IID_ITfCompositionSink) {
      *ppv = static_cast<ITfCompositionSink*>(this);
    } else if (riid == IID_ITfDisplayAttributeProvider) {
      *ppv = static_cast<ITfDisplayAttributeProvider*>(this);
    } else if (riid == IID_ITfCompartmentEventSink) {
      *ppv = static_cast<ITfCompartmentEventSink*>(this);
    } else {
      return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
  });
}

STDMETHODIMP_(ULONG) TextService::AddRef() { return InterlockedIncrement(&refCount_); }

STDMETHODIMP_(ULONG) TextService::Release() {
  LONG count = InterlockedDecrement(&refCount_);
  if (count == 0) delete this;
  return count;
}

STDMETHODIMP TextService::Activate(ITfThreadMgr* threadMgr, TfClientId clientId) {
  return Guarded("Activate", HRESULT(E_FAIL), [&]() -> HRESULT {
    return ActivateEx(threadMgr, clientId, 0);
  });
}

STDMETHODIMP TextService::ActivateEx(ITfThreadMgr* threadMgr, TfClientId clientId, DWORD) {
  return Guarded("ActivateEx", HRESULT(E_FAIL), [&]() -> HRESULT {
    threadMgr_ = threadMgr;
    clientId_ = clientId;

    ComPtr<ITfCategoryMgr> categoryMgr;
    if (SUCCEEDED(CoCreateInstance(CLSID_TF_CategoryMgr, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(&categoryMgr)))) {
      categoryMgr->RegisterGUID(GUID_DisplayAttributeInput, &inputAttributeAtom_);
      categoryMgr->RegisterGUID(GUID_DisplayAttributeError, &errorAttributeAtom_);
    }

    // Mouse actions arrive outside a key event, so they need their own edit sessions.
    candidateWindow_ = std::make_unique<CandidateWindow>(CandidateWindow::Handlers{
        [this](size_t index) {
          if (!compositionContext_ || !composition_) return;
          ComPtr<ITfContext> context = compositionContext_;
          ComPtr<ITfComposition> composition = composition_;
          ComPtr<TextService> self(this);
          EditSession::Request(context.Get(), clientId_, TF_ES_ASYNCDONTCARE | TF_ES_READWRITE,
                               [self, context, composition, index](TfEditCookie ec) {
                                 if (self->composition_ == composition &&
                                     self->compositionContext_ == context) {
                                   self->SelectCandidate(context.Get(), ec, index);
                                 }
                                 return S_OK;
                               });
        },
        [this] { OpenEmojiPanel(); },
  });

  engine_ = std::make_unique<EngineClient>(EngineClient::Handlers{
      [this](const protocol::RankResponse& response) { OnRankResponse(response); },
      [this](const protocol::UserDictResponse& response) { OnUserDictResponse(response); },
      [this](const protocol::Ack& ack) { OnAck(ack); },
  });
  ApplyUserDictionary();
  RequestUserDictionary();

  // The taskbar button is optional; input still works without it.
  InitLangBar();
  if (!InitThreadMgrEventSink() || !InitKeyEventSink() || !InitInputMode()) {
    Deactivate();
    return E_FAIL;
  }
  ComPtr<ITfDocumentMgr> focus;
  if (SUCCEEDED(threadMgr_->GetFocus(&focus))) RequestPrivacyUpdate(focus.Get());
  return S_OK;
  });
}

STDMETHODIMP TextService::Deactivate() {
  return Guarded("Deactivate", HRESULT(E_FAIL), [&]() -> HRESULT {
    if (composition_ && compositionContext_) {
      ComPtr<ITfContext> context = compositionContext_;
      EditSession::Request(context.Get(), clientId_, TF_ES_SYNC | TF_ES_READWRITE,
                           [this, context](TfEditCookie ec) {
                             CommitAll(context.Get(), ec);
                             return S_OK;
                           });
    }
    ResetState();
    engine_.reset();
    candidateWindow_.reset();
    UninitInputMode();
    UninitLangBar();
    UninitKeyEventSink();
    UninitThreadMgrEventSink();
    threadMgr_.Reset();
    clientId_ = TF_CLIENTID_NULL;
    return S_OK;
  });
}

bool TextService::InitThreadMgrEventSink() {
  ComPtr<ITfSource> source;
  if (FAILED(threadMgr_.As(&source))) return false;
  return SUCCEEDED(source->AdviseSink(IID_ITfThreadMgrEventSink,
                                      static_cast<ITfThreadMgrEventSink*>(this),
                                      &threadMgrEventSinkCookie_));
}

void TextService::UninitThreadMgrEventSink() {
  if (threadMgrEventSinkCookie_ == TF_INVALID_COOKIE || !threadMgr_) return;
  ComPtr<ITfSource> source;
  if (SUCCEEDED(threadMgr_.As(&source))) source->UnadviseSink(threadMgrEventSinkCookie_);
  threadMgrEventSinkCookie_ = TF_INVALID_COOKIE;
}

STDMETHODIMP TextService::OnInitDocumentMgr(ITfDocumentMgr*) { return S_OK; }
STDMETHODIMP TextService::OnUninitDocumentMgr(ITfDocumentMgr* docMgr) {
  return Guarded("OnUninitDocumentMgr", HRESULT(E_FAIL), [&]() -> HRESULT {
    // The document is going away with our composition in it: forget it, never edit it.
    ComPtr<ITfDocumentMgr> owner;
    if (compositionContext_ && SUCCEEDED(compositionContext_->GetDocumentMgr(&owner)) &&
        owner.Get() == docMgr) {
      ResetState();
    }
    return S_OK;
  });
}

STDMETHODIMP TextService::OnSetFocus(ITfDocumentMgr* focus, ITfDocumentMgr*) {
  return Guarded("OnSetFocus", HRESULT(E_FAIL), [&]() -> HRESULT {
    CloseCandidates();
    // Focus left the document we are composing in and the app did not end the composition:
    // commit it there (later, in that document's own edit session).
    ComPtr<ITfDocumentMgr> owner;
    if (composition_ && compositionContext_ &&
        SUCCEEDED(compositionContext_->GetDocumentMgr(&owner)) && owner.Get() != focus) {
      ComPtr<ITfContext> context = compositionContext_;
      ComPtr<ITfComposition> composition = composition_;
      ComPtr<TextService> self(this);
      EditSession::Request(context.Get(), clientId_, TF_ES_ASYNCDONTCARE | TF_ES_READWRITE,
                           [self, context, composition](TfEditCookie ec) {
                             if (self->composition_ == composition &&
                                 self->compositionContext_ == context) {
                               self->CommitAll(context.Get(), ec);
                             }
                             return S_OK;
                           });
    }
    RequestPrivacyUpdate(focus);
    return S_OK;
  });
}

STDMETHODIMP TextService::OnPushContext(ITfContext* context) {
  return Guarded("OnPushContext", HRESULT(E_FAIL), [&]() -> HRESULT {
    ComPtr<ITfDocumentMgr> docMgr;
    if (context && SUCCEEDED(context->GetDocumentMgr(&docMgr))) RequestPrivacyUpdate(docMgr.Get());
    return S_OK;
  });
}
STDMETHODIMP TextService::OnPopContext(ITfContext* context) {
  return Guarded("OnPopContext", HRESULT(E_FAIL), [&]() -> HRESULT {
    if (context && context == compositionContext_.Get()) ResetState();
    return S_OK;
  });
}

STDMETHODIMP TextService::EnumDisplayAttributeInfo(IEnumTfDisplayAttributeInfo** enumInfo) {
  return Guarded("EnumDisplayAttributeInfo", HRESULT(E_FAIL), [&]() -> HRESULT {
    return CreateDisplayAttributeEnum(enumInfo);
  });
}

STDMETHODIMP TextService::GetDisplayAttributeInfo(REFGUID guid, ITfDisplayAttributeInfo** info) {
  return Guarded("GetDisplayAttributeInfo", HRESULT(E_FAIL), [&]() -> HRESULT {
    return CreateDisplayAttributeInfo(guid, info);
  });
}

}  // namespace llaime
