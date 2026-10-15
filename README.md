# LLaIME

以本地小型語言模型自動選字的 Windows 11 中文輸入法（注音為主），介面復刻微軟注音的 Fluent 風格。

## 結構

| 路徑 | 說明 |
|---|---|
| `src/Tip` | TSF 輸入法 DLL（C++，x64 + x86） |
| `src/Engine` | 選字引擎背景程序（C++，詞庫 lattice + llama.cpp） |
| `src/Common` | 共用標頭 |
| `src/Settings` | 設定程式（WinUI 3） |
| `tools` | Python 工具（uv 專案）：詞庫建置、評測、訓練 |
| `scripts` | 開發用腳本 |

## 開發環境

- Visual Studio 2026（C++ 桌面開發、WinUI 應用程式開發）、Windows SDK 10.0.26100
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
```

## 安裝（開發用）

```powershell
powershell -ExecutionPolicy Bypass -File scripts\dev-install.ps1          # 安裝並註冊（會跳出 UAC）
powershell -ExecutionPolicy Bypass -File scripts\dev-install.ps1 -Uninstall
```

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
uv run python prepare_lexicon.py   # 下載資料來源並產生 data\build\lexicon.bin
```

| 來源 | 授權 | 用途 |
|---|---|---|
| [McBopomofo](https://github.com/openvanilla/McBopomofo) | MIT | 主要詞庫與詞頻 |
| [libchewing-data](https://github.com/chewing/libchewing-data) | LGPL-2.1-or-later | 補充詞（附詞頻） |
| 教育部《重編國語辭典修訂本》 | CC BY-ND 3.0 TW | 補充詞條與讀音 |

補充詞只收 McBopomofo 沒有的詞，原有的讀音與分數不變。教育部辭典採「禁止改作」授權，本專案不附原始檔，由腳本自行下載；若要散布含這些詞條的 `lexicon.bin`，需先確認是否符合授權條款。

## 選字模型

`%LOCALAPPDATA%\LLaIME\engine.ini` 的 `[model] ranker`：

| 值 | 說明 |
|---|---|
| `auto`（預設） | 有 GPU 用 `llm`，否則用 `laya` |
| `llm` | 因果語言模型（llama.cpp，`[model] path` 指定 GGUF），有顯示卡時最準 |
| `laya` | CPU 模式：微調過的 Laya 排序器（ONNX Runtime，`[laya] path` 指定 `models\laya-zh`），純 CPU 每次約 50 ms |

Laya 模型由 `tools/prune_laya_vocab.py` 與 `tools/export_laya_tokenizer.py` 從微調後的檢查點產生（見 `tests/eval/RESULTS.md`）。

## 使用者詞庫與學習

存放在 `%LOCALAPPDATA%\LLaIME\user.db`（SQLite），由選字引擎讀寫，各應用程式裡的輸入法透過 pipe 取得最新內容。

| 操作 | 說明 |
|---|---|
| 從候選清單改選 | 送出時記下「這個讀音選了這個詞」（也記前一個詞）。下次同讀音優先出現，整句排序時加分；權重每 30 天減半，改選別的詞時舊的減半 |
| 一個字一個字選出同一串字 | 連續兩次以上，自動成為學習詞 |
| `Shift+←／→` 選取，再按 `Ctrl+=` | 把選取的字（1–9 字）加入使用者詞庫；沒有選取時加入整段組字 |
| 工作列「中／英」按鈕右鍵 →「暫停學習」 | 暫停自動學習（手動加詞仍可用） |

密碼欄位與無痕視窗（私密模式）完全不學習。

學習效果可用 `LLaIME.LearnSim` 模擬（使用者遇到錯字就從候選改選，再測同一批句子與另一批句子），結果見 `tests/eval/RESULTS.md`。
