#include "TextService.h"

#include <filesystem>
#include <mutex>

#include "DisplayAttribute.h"
#include "EditSession.h"
#include "Globals.h"
#include "ui/CandidateWindow.h"

using Microsoft::WRL::ComPtr;

namespace llaime {
namespace {

std::shared_ptr<const BopomofoLayout> StandardLayout() {
  static std::shared_ptr<const BopomofoLayout> layout = CreateStandardLayout();
  return layout;
}

// One memory-mapped lexicon per process, looked up next to the installed DLL:
// <root>\x64\LLaIME.Tip.dll -> <root>\data\lexicon.bin
const Lexicon* SharedLexicon() {
  static Lexicon lexicon;
  static std::once_flag once;
  std::call_once(once, [] {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(g_hInst, path, MAX_PATH);
    std::filesystem::path dll(path);
    for (const auto& candidate : {dll.parent_path().parent_path() / L"data" / L"lexicon.bin",
                                  dll.parent_path() / L"data" / L"lexicon.bin"}) {
      if (lexicon.Open(candidate.wstring())) break;
    }
  });
  return lexicon.IsOpen() ? &lexicon : nullptr;
}

}  // namespace

TextService::TextService()
    : lexicon_(SharedLexicon()), reading_(StandardLayout()), composer_(lexicon_) {
  DllAddRef();
}

TextService::~TextService() { DllRelease(); }

STDMETHODIMP TextService::QueryInterface(REFIID riid, void** ppv) {
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
}

STDMETHODIMP_(ULONG) TextService::AddRef() { return InterlockedIncrement(&refCount_); }

STDMETHODIMP_(ULONG) TextService::Release() {
  LONG count = InterlockedDecrement(&refCount_);
  if (count == 0) delete this;
  return count;
}

STDMETHODIMP TextService::Activate(ITfThreadMgr* threadMgr, TfClientId clientId) {
  return ActivateEx(threadMgr, clientId, 0);
}

STDMETHODIMP TextService::ActivateEx(ITfThreadMgr* threadMgr, TfClientId clientId, DWORD) {
  threadMgr_ = threadMgr;
  clientId_ = clientId;

  ComPtr<ITfCategoryMgr> categoryMgr;
  if (SUCCEEDED(CoCreateInstance(CLSID_TF_CategoryMgr, nullptr, CLSCTX_INPROC_SERVER,
                                 IID_PPV_ARGS(&categoryMgr)))) {
    categoryMgr->RegisterGUID(GUID_DisplayAttributeInput, &inputAttributeAtom_);
  }

  candidateWindow_ = std::make_unique<CandidateWindow>([this](size_t index) {
    // Mouse selection arrives outside a key event, so it needs its own edit session.
    if (!compositionContext_) return;
    ComPtr<ITfContext> context = compositionContext_;
    EditSession::Request(context.Get(), clientId_, TF_ES_ASYNCDONTCARE | TF_ES_READWRITE,
                         [this, context, index](TfEditCookie ec) {
                           SelectCandidate(context.Get(), ec, index);
                           return S_OK;
                         });
  });

  if (!InitThreadMgrEventSink() || !InitKeyEventSink() || !InitInputMode()) {
    Deactivate();
    return E_FAIL;
  }
  return S_OK;
}

STDMETHODIMP TextService::Deactivate() {
  if (composition_ && compositionContext_) {
    ComPtr<ITfContext> context = compositionContext_;
    EditSession::Request(context.Get(), clientId_, TF_ES_SYNC | TF_ES_READWRITE,
                         [this, context](TfEditCookie ec) {
                           CommitAll(context.Get(), ec);
                           return S_OK;
                         });
  }
  ResetState();
  candidateWindow_.reset();
  UninitInputMode();
  UninitKeyEventSink();
  UninitThreadMgrEventSink();
  threadMgr_.Reset();
  clientId_ = TF_CLIENTID_NULL;
  return S_OK;
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
STDMETHODIMP TextService::OnUninitDocumentMgr(ITfDocumentMgr*) { return S_OK; }

STDMETHODIMP TextService::OnSetFocus(ITfDocumentMgr*, ITfDocumentMgr*) {
  CloseCandidates();
  return S_OK;
}

STDMETHODIMP TextService::OnPushContext(ITfContext*) { return S_OK; }
STDMETHODIMP TextService::OnPopContext(ITfContext*) { return S_OK; }

STDMETHODIMP TextService::EnumDisplayAttributeInfo(IEnumTfDisplayAttributeInfo** enumInfo) {
  return CreateDisplayAttributeEnum(enumInfo);
}

STDMETHODIMP TextService::GetDisplayAttributeInfo(REFGUID guid, ITfDisplayAttributeInfo** info) {
  return CreateDisplayAttributeInfo(guid, info);
}

}  // namespace llaime
