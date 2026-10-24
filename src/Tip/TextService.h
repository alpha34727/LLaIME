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
// How many characters before and after the composition the ranker gets (the settings).
struct ContextChars {
  uint32_t before;
  uint32_t after;
};
ContextChars CurrentContextChars();
// 0-100: how much the language model writes the conversion itself (the settings; 0: ranking).
uint32_t CurrentLlmWriting();

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
    // User phrase shortcuts: ` then p starts one, then its keys (e.g. ` p m k -> 「」).
    ShortcutStart,
    ShortcutKey,  // `ch`: a letter or digit
    ShortcutBackspace,
    ShortcutAccept,  // Enter / Space: the phrase of exactly the keys typed
    ShortcutCancel,
    ShortcutReject,  // any other key: beep
  };
  Type type = Type::PassThrough;
  wchar_t ch = 0;
  std::wstring text;
  int number = 0;
};

class TextService final : public ITfTextInputProcessorEx,
                          public ITfThreadMgrEventSink,
                          public ITfThreadFocusSink,
                          public ITfKeyEventSink,
                          public ITfCompositionSink,
                          public ITfDisplayAttributeProvider,
                          public ITfCompartmentEventSink,
                          public ITfKeyTraceEventSink,
                          public ITfCandidateListUIElement {
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

  // ITfThreadFocusSink
  STDMETHODIMP OnSetThreadFocus() override;
  STDMETHODIMP OnKillThreadFocus() override;

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

  // ITfCandidateListUIElement (CandidateUi.cpp): lets apps that draw the candidates
  // themselves (Windows Search, Sticky Notes) show them.
  STDMETHODIMP GetDescription(BSTR* description) override;
  STDMETHODIMP GetGUID(GUID* guid) override;
  STDMETHODIMP Show(BOOL show) override;
  STDMETHODIMP IsShown(BOOL* shown) override;
  STDMETHODIMP GetUpdatedFlags(DWORD* flags) override;
  STDMETHODIMP GetDocumentMgr(ITfDocumentMgr** docMgr) override;
  STDMETHODIMP GetCount(UINT* count) override;
  STDMETHODIMP GetSelection(UINT* index) override;
  STDMETHODIMP GetString(UINT index, BSTR* text) override;
  STDMETHODIMP GetPageIndex(UINT* index, UINT size, UINT* pageCount) override;
  STDMETHODIMP SetPageIndex(UINT* index, UINT pageCount) override;
  STDMETHODIMP GetCurrentPage(UINT* page) override;

  // ITfKeyTraceEventSink (BypassedKeys.cpp)
  STDMETHODIMP OnKeyTraceDown(WPARAM wParam, LPARAM lParam) override;
  STDMETHODIMP OnKeyTraceUp(WPARAM wParam, LPARAM lParam) override;

 private:
  ~TextService();

  bool InitThreadMgrEventSink();
  void UninitThreadMgrEventSink();
  bool InitKeyEventSink();
  void UninitKeyEventSink();

  // Keys the app handled without offering them to the IME (BypassedKeys.cpp).
  bool InitKeyTrace();
  void UninitKeyTrace();
  void NoteKeyOffered(WPARAM vk);
  void DoBypassedKey();
  void ApplyBypassedKey(ITfContext* context, TfEditCookie ec, bool last);
  static constexpr int kBypassedAttempts = 5;
  static bool TakeBackTyped(ITfContext* context, TfEditCookie ec, wchar_t typed);
  static LRESULT CALLBACK KeyWatchProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

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
  // The document text before and after the composition (contextText_, followingText_).
  void ReadContext(ITfContext* context, TfEditCookie ec, ITfRange* compositionRange);
  void RequestRanking();
  bool DecodedSentence(const protocol::RankResponse& response, Sentence* out) const;
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
  bool IsComposing() const {
    return !composer_.empty() || !reading_.Empty() || shortcutMode_;
  }
  // Shortcut.cpp
  KeyAction ClassifyShortcutKey(WPARAM vk) const;
  bool AtShortcutTrigger() const;
  void HandleShortcut(ITfContext* context, TfEditCookie ec, const KeyAction& action);
  // Whether an edit session on `context` may touch our composition. Apps (Word saving,
  // Chromium moving focus) can replace the document's context without ending the
  // composition; a range of one context must never be edited with another's cookie, so a
  // composition left in another context is dropped (its text stays where it was).
  bool CompositionUsable(ITfContext* context);
  void CommitOnFocusLoss();

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
  // CandidateUi.cpp: tells the app about the candidate list (begin, change, end).
  void BeginCandidateUi();
  void UpdateCandidateUi();
  void EndCandidateUi();
  DWORD candidateUiId_ = 0;
  bool candidateUiActive_ = false;

  LONG refCount_ = 1;
  Microsoft::WRL::ComPtr<ITfThreadMgr> threadMgr_;
  TfClientId clientId_ = TF_CLIENTID_NULL;
  DWORD threadMgrEventSinkCookie_ = TF_INVALID_COOKIE;
  DWORD threadFocusSinkCookie_ = TF_INVALID_COOKIE;
  DWORD conversionSinkCookie_ = TF_INVALID_COOKIE;
  DWORD openCloseSinkCookie_ = TF_INVALID_COOKIE;
  TfGuidAtom inputAttributeAtom_ = TF_INVALID_GUIDATOM;
  TfGuidAtom errorAttributeAtom_ = TF_INVALID_GUIDATOM;

  bool chineseMode_ = true;
  bool fullShape_ = false;
  bool privateMode_ = false;
  bool updatingCompartment_ = false;
  bool shiftAlone_ = false;
  // BypassedKeys.cpp
  struct BypassedKey {
    WPARAM vk = 0;
    KeyAction action;
    wchar_t typed = 0;  // what the app typed for it, if anything
  };
  HWND keyWatchWindow_ = nullptr;
  DWORD keyTraceCookie_ = TF_INVALID_COOKIE;
  WPARAM lastOfferedVk_ = 0;
  BypassedKey bypassed_;
  bool bypassedPending_ = false;
  int bypassedAttempts_ = 0;
  bool bypassedConfirmed_ = false;  // the app handled the key message without offering it
  bool readingInvalid_ = false;  // last tone key completed a syllable that does not exist
  bool shortcutMode_ = false;    // after ` p: typing a user phrase's shortcut keys
  std::wstring shortcutKeys_;

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
  // The lattice of the last request when the model writes the conversion (empty otherwise),
  // and the lexicon weight used.
  std::vector<Node> decodeLattice_;
  double decodeLexiconWeight_ = 0;
  std::wstring contextText_;    // document text before the composition
  std::wstring followingText_;  // and after it

  std::shared_ptr<const UserDictionary> userDict_;
  ULONGLONG userDictRequestTick_ = 0;  // a request is on its way since then (0: none)
  bool learningEnabled_ = true;

  Microsoft::WRL::ComPtr<ITfComposition> composition_;
  Microsoft::WRL::ComPtr<ITfContext> compositionContext_;
};

}  // namespace llaime
