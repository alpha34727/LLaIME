#pragma once
#include <msctf.h>
#include <wrl/client.h>

#include <memory>
#include <string>
#include <vector>

#include "llaime/BopomofoReading.h"
#include "llaime/Composer.h"

namespace llaime {

class CandidateWindow;
class LangBarButton;
class ThemeWatcher;

// What a key press does, decided in OnTestKeyDown and carried out in OnKeyDown.
struct KeyAction {
  enum class Type {
    PassThrough,
    Swallow,
    Reading,      // feed `ch` to the Bopomofo reading
    Literal,      // insert `text` (punctuation, Shift+letter) into the buffer
    Backspace,
    Delete,
    MoveLeft,
    MoveRight,
    MoveHome,
    MoveEnd,
    Commit,
    Cancel,
    OpenCandidates,
    CandidateDigit,  // `number` = 1..9
    CandidateUp,
    CandidateDown,
    CandidatePageUp,
    CandidatePageDown,
    CandidateAccept,
    CandidateClose,
    ToggleShape,   // Shift+Space: half/full width
    DirectInsert,  // insert `text` straight into the document (nothing is being composed)
  };
  Type type = Type::PassThrough;
  wchar_t ch = 0;
  std::wstring text;
  int number = 0;
};

class TextService final : public ITfTextInputProcessorEx,
                          public ITfThreadMgrEventSink,
                          public ITfKeyEventSink,
                          public ITfCompositionSink,
                          public ITfDisplayAttributeProvider,
                          public ITfCompartmentEventSink {
 public:
  TextService();

  // IUnknown
  STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
  STDMETHODIMP_(ULONG) AddRef() override;
  STDMETHODIMP_(ULONG) Release() override;

  // ITfTextInputProcessor / Ex
  STDMETHODIMP Activate(ITfThreadMgr* threadMgr, TfClientId clientId) override;
  STDMETHODIMP Deactivate() override;
  STDMETHODIMP ActivateEx(ITfThreadMgr* threadMgr, TfClientId clientId, DWORD flags) override;

  // ITfThreadMgrEventSink
  STDMETHODIMP OnInitDocumentMgr(ITfDocumentMgr* docMgr) override;
  STDMETHODIMP OnUninitDocumentMgr(ITfDocumentMgr* docMgr) override;
  STDMETHODIMP OnSetFocus(ITfDocumentMgr* focus, ITfDocumentMgr* prevFocus) override;
  STDMETHODIMP OnPushContext(ITfContext* context) override;
  STDMETHODIMP OnPopContext(ITfContext* context) override;

  // ITfKeyEventSink (KeyEventSink.cpp)
  STDMETHODIMP OnSetFocus(BOOL foreground) override;
  STDMETHODIMP OnTestKeyDown(ITfContext* context, WPARAM wParam, LPARAM lParam,
                             BOOL* eaten) override;
  STDMETHODIMP OnKeyDown(ITfContext* context, WPARAM wParam, LPARAM lParam, BOOL* eaten) override;
  STDMETHODIMP OnTestKeyUp(ITfContext* context, WPARAM wParam, LPARAM lParam, BOOL* eaten) override;
  STDMETHODIMP OnKeyUp(ITfContext* context, WPARAM wParam, LPARAM lParam, BOOL* eaten) override;
  STDMETHODIMP OnPreservedKey(ITfContext* context, REFGUID guid, BOOL* eaten) override;

  // ITfCompositionSink
  STDMETHODIMP OnCompositionTerminated(TfEditCookie ec, ITfComposition* composition) override;

  // ITfDisplayAttributeProvider
  STDMETHODIMP EnumDisplayAttributeInfo(IEnumTfDisplayAttributeInfo** enumInfo) override;
  STDMETHODIMP GetDisplayAttributeInfo(REFGUID guid, ITfDisplayAttributeInfo** info) override;

  // ITfCompartmentEventSink (InputMode.cpp)
  STDMETHODIMP OnChange(REFGUID guid) override;

 private:
  ~TextService();

  bool InitThreadMgrEventSink();
  void UninitThreadMgrEventSink();
  bool InitKeyEventSink();
  void UninitKeyEventSink();

  // Input mode (InputMode.cpp)
  bool InitInputMode();
  void UninitInputMode();
  void SetChineseMode(bool chinese);
  void SetFullShape(bool full);
  void WriteConversionMode();
  // Toggles from the taskbar button: commits any pending composition first.
  void ToggleChineseFromUi();
  bool InitLangBar();
  void UninitLangBar();

  // Key handling (KeyEventSink.cpp)
  KeyAction ClassifyKey(WPARAM vk) const;
  HRESULT ExecuteKeyAction(ITfContext* context, const KeyAction& action);
  void HandleAction(ITfContext* context, TfEditCookie ec, const KeyAction& action);
  bool IsComposing() const { return !composer_.empty() || !reading_.Empty(); }

  // Composition (Composition.cpp)
  void UpdateComposition(ITfContext* context, TfEditCookie ec);
  void CommitAll(ITfContext* context, TfEditCookie ec);
  void InsertDirect(ITfContext* context, TfEditCookie ec, const std::wstring& text);
  void CommitOverflow(ITfContext* context, TfEditCookie ec);
  void CancelComposition(ITfContext* context, TfEditCookie ec);
  bool StartComposition(ITfContext* context, TfEditCookie ec);
  void EndComposition(TfEditCookie ec);
  void ApplyDisplayAttribute(ITfContext* context, TfEditCookie ec, ITfRange* range,
                             TfGuidAtom atom);
  bool GetTextRect(ITfContext* context, TfEditCookie ec, size_t begin, size_t end, RECT* rect);
  void ResetState();

  // Candidates (Composition.cpp)
  void OpenCandidates(ITfContext* context, TfEditCookie ec);
  void SelectCandidate(ITfContext* context, TfEditCookie ec, size_t index);
  void CloseCandidates();
  bool CandidatesOpen() const;

  LONG refCount_ = 1;
  Microsoft::WRL::ComPtr<ITfThreadMgr> threadMgr_;
  TfClientId clientId_ = TF_CLIENTID_NULL;
  DWORD threadMgrEventSinkCookie_ = TF_INVALID_COOKIE;
  DWORD conversionSinkCookie_ = TF_INVALID_COOKIE;
  DWORD openCloseSinkCookie_ = TF_INVALID_COOKIE;
  TfGuidAtom inputAttributeAtom_ = TF_INVALID_GUIDATOM;
  TfGuidAtom errorAttributeAtom_ = TF_INVALID_GUIDATOM;

  bool chineseMode_ = true;
  bool fullShape_ = false;
  bool updatingCompartment_ = false;
  bool shiftAlone_ = false;
  bool readingInvalid_ = false;  // last tone key completed a syllable that does not exist

  const Lexicon* lexicon_;
  BopomofoReading reading_;
  Composer composer_;
  std::vector<Candidate> candidates_;
  std::unique_ptr<CandidateWindow> candidateWindow_;
  LangBarButton* langBarButton_ = nullptr;
  std::unique_ptr<ThemeWatcher> themeWatcher_;  // refreshes the 中/英 icon on theme change

  Microsoft::WRL::ComPtr<ITfComposition> composition_;
  Microsoft::WRL::ComPtr<ITfContext> compositionContext_;
};

}  // namespace llaime
