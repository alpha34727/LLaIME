#pragma once
#include <msctf.h>
#include <wrl/client.h>

#include <memory>
#include <string>
#include <vector>

#include "llaime/BopomofoReading.h"
#include "llaime/Composer.h"
#include "llaime/Protocol.h"

namespace llaime {

// Lexicons.cpp: the process-wide lexicon with the layers the settings enable.
std::shared_ptr<const Lexicon> CurrentLexicon();

class CandidateWindow;
class LangBarButton;
class ThemeWatcher;
class EngineClient;

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
    CandidateLeft,    // previous column or collapse (expanded), previous page (list)
    CandidateRight,   // expand (list), next column wrapping to the first (expanded)
    CandidateToggleExpanded,
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
  static void OpenSettings();
  bool InitLangBar();
  void UninitLangBar();

  // Privacy mode (Privacy.cpp): password fields and private browsing. While on, nothing may
  // be learned or logged.
  void UpdatePrivacy(ITfContext* context, TfEditCookie ec);
  void RequestPrivacyUpdate(ITfDocumentMgr* docMgr);
  void SetPrivateMode(bool isPrivate);
  bool IsPrivateMode() const { return privateMode_; }

  // Language-model reranking (Ranking.cpp)
  void ReadContextBefore(ITfContext* context, TfEditCookie ec, ITfRange* compositionRange);
  void RequestRanking();
  // Scores the sentence each open candidate would produce; the results become the
  // probabilities shown in the candidate window. Returns false if no request was sent.
  bool RequestCandidateProbabilities();
  void OnRankResponse(const protocol::RankResponse& response);
  void OnCandidateProbabilities(const protocol::RankResponse& response);

  // User dictionary and learning (UserDict.cpp)
  void RequestUserDictionary();
  // Fetches the user dictionary if the engine's `version` is newer than the one held.
  void CheckUserVersion(uint32_t version);
  void OnUserDictResponse(const protocol::UserDictResponse& response);
  void OnAck(const protocol::Ack& ack);
  // Takes the process's newest snapshot, unless composing.
  void ApplyUserDictionary();
  // Lexicons.cpp: switches to the layers the settings now enable (not while composing).
  void ApplyLexicon();
  // Sends what committing elements [0, upTo) of the buffer taught.
  void LearnFromCommit(size_t upTo);
  void SetLearningEnabled(bool enabled);

  // Key handling (KeyEventSink.cpp)
  KeyAction ClassifyKey(WPARAM vk) const;
  HRESULT ExecuteKeyAction(ITfContext* context, const KeyAction& action);
  void HandleAction(ITfContext* context, TfEditCookie ec, const KeyAction& action);
  bool IsComposing() const { return !composer_.empty() || !reading_.Empty(); }
  // Whether an edit session on `context` may touch our composition. Apps (Word saving,
  // Chromium moving focus) can replace the document's context without ending the
  // composition; a range of one context must never be edited with another's cookie, so a
  // composition left in another context is dropped (its text stays where it was).
  bool CompositionUsable(ITfContext* context);

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
  // Commits the composition and opens the Windows emoji panel (the footer emoji button).
  void OpenEmojiPanel();
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
  bool privateMode_ = false;
  bool updatingCompartment_ = false;
  bool shiftAlone_ = false;
  bool readingInvalid_ = false;  // last tone key completed a syllable that does not exist

  std::shared_ptr<const Lexicon> lexiconRef_;  // keeps the layers mapped while in use
  const Lexicon* lexicon_;
  BopomofoReading reading_;
  Composer composer_;
  std::vector<Candidate> candidates_;
  std::unique_ptr<CandidateWindow> candidateWindow_;
  LangBarButton* langBarButton_ = nullptr;
  std::unique_ptr<ThemeWatcher> themeWatcher_;  // refreshes the 中/英 icon on theme change

  std::unique_ptr<EngineClient> engine_;
  uint32_t lastRequestId_ = 0;
  uint64_t rankedGeneration_ = 0;
  std::vector<Sentence> rankedSentences_;
  // Candidate probabilities: the distinct sentences sent, and for each candidate the index of
  // its sentence (-1 past the request's limit).
  uint32_t candidateRequestId_ = 0;
  std::vector<Sentence> candidateSentences_;
  std::vector<int> candidateSentenceIndex_;
  std::wstring contextText_;  // document text before the composition

  std::shared_ptr<const UserDictionary> userDict_;
  ULONGLONG userDictRequestTick_ = 0;  // a request is on its way since then (0: none)
  bool learningEnabled_ = true;

  Microsoft::WRL::ComPtr<ITfComposition> composition_;
  Microsoft::WRL::ComPtr<ITfContext> compositionContext_;
};

}  // namespace llaime
