# LLaIME

以本地小型語言模型自動選字的 Windows 11 中文輸入法（注音為主），介面復刻微軟注音的 Fluent 風格。

## 結構

| 路徑 | 說明 |
|---|---|
| `src/Tip` | TSF 輸入法 DLL（C++，x64 + x86） |
| `src/Engine` | 選字引擎背景程序（C++，詞庫 lattice + llama.cpp） |
| `src/Common` | 共用標頭 |
| `src/Settings` | 設定程式：`App`（WinUI 3，C#）＋ `Native`（C API，重用輸入法與引擎的 C++ 程式） |
| `tools` | Python 工具（uv 專案）：詞庫建置、評測、訓練 |
| `scripts` | 開發用腳本 |

## 開發環境

- Visual Studio 2026（C++ 桌面開發）、Windows SDK 10.0.26100
- .NET 10 SDK（設定程式；Windows App SDK 等套件由 NuGet 取得）
- CMake 4.x（`uv tool install cmake`）
- uv（Python 工具環境，含 CUDA 版 PyTorch）
- CUDA Toolkit 13.x（NVIDIA 加速）、Vulkan SDK（Intel／AMD 內顯加速）

## 建置

選字引擎需要 llama.cpp 與 ONNX Runtime（先各執行一次）：

```powershell
powershell -ExecutionPolicy Bypass -File scripts\build-llama.ps1
powershell -ExecutionPolicy Bypass -File scripts\fetch-onnxruntime.ps1
```

```powershell
cmake --preset x64; cmake --build --preset x64-debug
cmake --preset x86; cmake --build --preset x86-debug
dotnet build src\Settings\App -c Release   # 設定程式（需先建好 x64 的 LLaIME.SettingsNative）
```

## 安裝（開發用）

```powershell
powershell -ExecutionPolicy Bypass -File scripts\dev-install.ps1          # 安裝並註冊（會跳出 UAC）
powershell -ExecutionPolicy Bypass -File scripts\dev-install.ps1 -Uninstall
```

安裝程式也會建置並安裝設定程式（`C:\Program Files\LLaIME\settings`），開始功能表出現「LLaIME 設定」。

安裝後到「設定 → 時間與語言 → 語言與地區 → 中文（台灣）→ 語言選項 → 鍵盤 → 新增鍵盤」加入「LLaIME 注音」。

## Python 工具

```powershell
cd tools
uv sync
uv run python make_icon.py ..\src\Tip\res\bopomofo.ico
```

## 詞庫

```powershell
cd tools
uv run python prepare_lexicon.py   # 下載資料來源並產生 data\build\lexicons\*.bin
```

每個來源各編成一個詞庫層（`.bin`，附 `.ini` 說明名稱與授權），可在設定程式的「詞庫」頁個別停用（McBopomofo 主詞庫固定啟用）。同一個詞出現在多層時取最高分。

| 來源 | 授權 | 用途 |
|---|---|---|
| [McBopomofo](https://github.com/openvanilla/McBopomofo) | MIT | 主要詞庫與詞頻 |
| [libchewing-data](https://github.com/chewing/libchewing-data) | LGPL-2.1-or-later | 補充詞（附詞頻） |
| 教育部《重編國語辭典修訂本》 | CC BY-ND 3.0 TW | 補充詞條與讀音 |

補充詞只收 McBopomofo 沒有的詞，原有的讀音與分數不變。教育部辭典採「禁止改作」授權，本專案不附原始檔，由腳本自行下載；若要散布 `moe.bin`，需先確認是否符合授權條款。

## 選字模型

在設定程式的「選字模型與加速」頁選擇（寫入 `%LOCALAPPDATA%\LLaIME\engine.ini`，套用時引擎重新載入模型，不必重開）。`[model] ranker`：

| 值 | 說明 |
|---|---|
| `auto`（預設） | 有 GPU 用 `llm`，否則用 `laya` |
| `llm` | 因果語言模型（llama.cpp，`[model] path` 指定 GGUF），有顯示卡時最準 |
| `laya` | CPU 模式：微調過的 Laya 排序器（ONNX Runtime，`[laya] path` 指定 `models\laya-zh`），純 CPU 每次約 50 ms |

`[model] device` 是加速裝置（`auto`、`CPU`、`CUDA0`、`Vulkan0`…，清單由引擎列舉），`[model] threads` 是 CPU 執行緒數（0 為自動）。同一頁的「測速」以目前的模型送出 30 次選字請求：Qwen3.5-2B 台灣 LoRA 在 RTX 5060 Ti（CUDA）中位數 33 ms，在 i5-12400（CPU）506 ms。

Laya 模型由 `tools/prune_laya_vocab.py` 與 `tools/export_laya_tokenizer.py` 從微調後的檢查點產生（見 `tests/eval/RESULTS.md`）。

## 使用者詞庫與學習

存放在 `%LOCALAPPDATA%\LLaIME\user.db`（SQLite），由選字引擎讀寫，各應用程式裡的輸入法透過 pipe 取得最新內容。

| 操作 | 說明 |
|---|---|
| 從候選清單改選 | 送出時記下「這個讀音選了這個詞」（也記前一個詞）。下次同讀音優先出現，整句排序時加分；權重每 30 天減半，改選別的詞時舊的減半 |
| 一個字一個字選出同一串字 | 連續兩次以上，自動成為學習詞 |
| 工作列「中／英」按鈕右鍵 →「暫停學習」 | 暫停自動學習 |

密碼欄位與無痕視窗（私密模式）完全不學習。設定程式的「使用者詞庫」頁可以：

- 新增、編輯、刪除詞彙（輸入詞後依詞庫自動標注音，破音字可從清單換讀音）；自動成詞可轉為手動詞；
- 為詞指定快捷鍵（英文字母與數字），輸入時依序按 `、p 再按快捷鍵就打出這個詞，例如把「」設成 `mk`，按 ` p m k 即輸入「」。快捷鍵與一般組字一樣有底線，按到沒有對應的鍵會發出提示音；有更長的快捷鍵以同樣開頭時（例如 `mk` 與 `mkk`），按 Enter 或空白鍵確定較短的那個。只用快捷鍵輸入的詞（如符號）可以不填注音；
- 檢視、刪除學習紀錄，或「清除全部學習紀錄」（手動詞保留；刪除的資料會被覆寫，不留在檔案裡）；
- 匯入詞彙（McBopomofo、新酷音 `tsi.csv`／`tsi.src`、一行一詞的純文字自動標音）與匯出（McBopomofo 使用者詞彙格式）。

設定程式直接讀寫 `user.db`；引擎偵測到變更後，各應用程式的輸入法在下一次組字時套用。

學習效果可用 `LLaIME.LearnSim` 模擬（使用者遇到錯字就從候選改選，再測同一批句子與另一批句子），結果見 `tests/eval/RESULTS.md`。

## 隱私模式

完全依照應用程式回報的欄位類型判斷（不看視窗標題）：TSF input scope 為 `IS_PASSWORD`、`IS_NUMERIC_PASSWORD`、PIN 類或 `IS_PRIVATE`（Chrome 無痕、Edge InPrivate、Firefox 私密視窗），或舊式 Win32 Edit 的 `ES_PASSWORD`。私密模式下：

- 工作列「中／英」圖示加上盾牌；
- 不學習、不送出上文（游標前的文字）給模型，引擎也不快取這些評分；
- 仍可正常用詞庫與模型選字。

端對端測試（會模擬按鍵，執行時請勿操作電腦；使用暫存的 `user.db`，不影響真正的詞庫）：

```powershell
build\x64\src\Engine\Release\LLaIME.PrivacyTest.exe              # 測試視窗內的四種欄位
build\x64\src\Engine\Release\LLaIME.PrivacyTest.exe --browsers   # 加上 Chrome／Edge／Firefox 一般、密碼、私密視窗
```

2026-10-15 結果：13 種情況全部通過（Win32 Edit、`ES_PASSWORD`、`IS_PASSWORD`、`IS_PRIVATE`，以及三種瀏覽器的文字欄、密碼欄、私密視窗）。

## 設定程式

`src/Settings/App`：WinUI 3（Windows App SDK 2.5，免安裝執行階段的自封裝版本）＋ Windows Community Toolkit 的 SettingsCard，外觀與 Windows 11 設定一致。頁面：一般（鍵盤排列、學習開關）、選字模型與加速（選字方式、模型、加速裝置、執行緒、測速）、詞庫（詞庫層開關、匯入、刪除）、使用者詞庫、關於。也可從工作列「中／英」按鈕右鍵選單的「設定」開啟。

輸入法讀取的設定（停用的詞庫層、匯入詞庫的資料夾）存在 `HKCU\Software\LLaIME`，該機碼與 `%LOCALAPPDATA%\LLaIME\lexicons` 都開放 `ALL APPLICATION PACKAGES` 讀取，所以 UWP／封裝程式裡的輸入法也讀得到。

端對端測試：在測試視窗裡打「ㄗㄨㄥˇ ㄓㄨㄤˋ ㄏㄨㄚ ㄒㄩˋ」，切換教育部詞庫層（不重開程式）看送出的字：

```powershell
build\x64\src\Engine\Release\LLaIME.LexiconTest.exe
```

2026-10-15 結果：啟用「總狀花序」、停用「總狀花絮」、再啟用「總狀花序」，全部通過。

快捷鍵的端對端測試（暫存的 `user.db` 裡把「」設成 `mk`、『』設成 `mkk`）：

```powershell
build\x64\src\Engine\Release\LLaIME.ShortcutTest.exe
```

2026-10-18 結果：` p m k Enter →「」、` p m k k →『』、組字中途插入（我『』）、沒有對應的鍵被忽略、Esc 取消、Backspace 退回 `，6 項全部通過。
