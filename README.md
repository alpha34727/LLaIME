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
