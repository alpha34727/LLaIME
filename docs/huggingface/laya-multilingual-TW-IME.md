---
language:
- zh
license: apache-2.0
base_model: convaiinnovations/laya-multilingual
library_name: transformers
pipeline_tag: text-classification
tags:
- laya
- mmbert
- onnx
- traditional-chinese
- zh-tw
- taiwan
- input-method
- bopomofo
- reranking
---

# laya-multilingual-TW-IME

[convaiinnovations/laya-multilingual](https://huggingface.co/convaiinnovations/laya-multilingual)（mmBERT-base 的非自回歸決策模型，322M）針對**注音輸入法的候選句排序**微調的版本：給它上文和一組同音的候選句，**一次前向傳遞**就給出每一句的機率。它是 [LLaIME](PROJECT_URL)（Windows 11 注音輸入法）沒有獨立顯卡時使用的「決策模型（System 1 Model）」，附上可在純 CPU 上跑的 INT8 ONNX 版本（162 MB，約 50 ms）。

> **先讀這幾點**
>
> - 只在「從詞庫候選句中挑出正確的一句」這一個任務上評測過；Laya 原本的其他功能沒有評測，微調後很可能退步。
> - **訓練資料全部可以自由再利用**：CC0、政府資料開放授權條款（與 CC BY 4.0 相容）、不受著作權保護的法規，以及由 Apache-2.0 模型在本機生成的句子；候選句由 McBopomofo（MIT）與新酷音（LGPL-2.1）詞庫產生。沒有使用維基百科與教育部辭典。來源、授權與雜湊值見 `DATA.md` 與 `sources.json`。
> - 準確率比同一專案的 [Qwen3.5-2B-TW-IME](https://huggingface.co/REPO_OWNER/Qwen3.5-2B-TW-IME) 低約 2–3 個百分點，優點是不需要顯示卡。
> - 輸入是**候選句**，模型看不到注音；換一種候選產生方式準確率會變。

## 用途與輸入格式

輸入法把打出的注音經詞庫轉成前 16 名候選整句，這個模型替每一句打分，再與詞庫分數混合。

| 欄位 | 內容 |
|---|---|
| state | `上文：` ＋ 游標前的文字（沒有上文就寫 `無`） |
| 題目 | `choice`，instructions 為「哪一個是這段注音最自然的台灣繁體中文寫法？」 |
| 選項 | 候選句，通常 2–16 句 |

## 使用方式（PyTorch）

```bash
pip install laya   # 設定環境變數 USE_TF=0
```

```python
import laya

agent = laya.load("REPO_ID")
candidates = ["排在第二順位先來解決", "牌在第二順位先來解決", "排再第二順位先來解決"]
answer = agent.predict(
    "上文：無",
    {"sentence": {
        "type": "choice",
        "instructions": "哪一個是這段注音最自然的台灣繁體中文寫法？",
        "criteria": {c: "" for c in candidates},
    }},
)["answers"]["sentence"]
print(answer["choice"], answer["probabilities"])
```

`probabilities` 已套用校準溫度（choice 為 2.60）。LLaIME 用未套溫度的選項 logits 做 log-softmax 再與詞庫分數混合；兩者排序相同。

## 使用方式（ONNX，CPU）

`laya.int8.onnx` 是縮小詞表並 INT8 量化的圖，用 ONNX Runtime 在 CPU 上執行。輸入為 `input_ids`、`attention_mask`、`marker_pos`、`marker_mask`、`qtype`（同 Laya 官方 `export_onnx.py`），輸出 `logits`。圖內有「原 mmBERT 詞元 ID → 保留詞元」的對照表，所以仍用原本的 `tokenizer/tokenizer.json` 斷詞。LLaIME 的 C++ 實作（`src/Engine/LayaTokenizer.cpp`、`LayaScorer.cpp`，搭配本 repo 的 `tokenizer.bin`）的輸入序列與 Laya 的 Python 版在全部 102,581 筆訓練題目上逐筆一致。

## 檔案

| 檔案 | 說明 |
|---|---|
| `model.safetensors`、`rl_agent_config.json`、`encoder/`、`tokenizer/`、`questions.json` | PyTorch 檢查點，用 `laya.load()` 讀取 |
| `train_report.json` | `laya-train` 的報告；其中的準確率是 400 題校準切片的**樣本內**數字，不是留出評測 |
| `laya.int8.onnx` | 縮小詞表＋INT8 的 ONNX（162 MB），CPU 用 |
| `laya.fp16.onnx` | 縮小詞表＋fp16 的 ONNX（299 MB），NVIDIA 顯示卡用（ONNX Runtime CUDA） |
| `tokenizer.bin` | 給 LLaIME 的 C++ 斷詞器 |
| `DATA.md`、`sources.json` | 訓練資料的出處、授權與 SHA-256 |
| `training/` | 資料下載、清理、生成、題目產生、訓練與匯出腳本；生成的句子與訓練記錄 |

## 訓練

**資料**：全部由文字自動產生，沒有人工標注。來源句切成 4–24 個漢字的片段，用 McBopomofo＋新酷音詞庫轉成注音，再讓同一份詞庫產生前 16 名候選整句；正解就是原本的片段。前 16 名沒有正解、或只有一個候選的題目丟棄。

| 來源 | 題數 | 授權 |
|---|---|---|
| Common Voice zh-TW 句子 | 13,959 | CC0 |
| 立法院公報（400 場院會與委員會紀錄的發言） | 36,728 | 政府資料開放授權條款－第1版 |
| 行政院即時新聞、雲嘉南區聯合服務中心焦點新聞 | 23,863 | 政府資料開放授權條款－第1版 |
| [Breeze-7B-Instruct-v1_0](https://huggingface.co/MediaTek-Research/Breeze-7B-Instruct-v1_0)（Apache-2.0）本機生成的日常口語句 | 22,222 | 隨本模型公開（`training/synthetic_sentences.txt`） |
| 全國法規資料庫（現行法規條文） | 5,809 | 不受著作權保護（著作權法第 9 條） |

共 102,581 題，其中 102,181 題訓練、400 題由 `laya-train` 留作校準。評測句與其所在的整句都排除在外。

**方法**（`laya-train` 0.4.7，`training/run-laya-free.cmd`）：

| 項目 | 設定 |
|---|---|
| 基底 | convaiinnovations/laya-multilingual（編碼器 jhu-clsp/mmBERT-base） |
| 損失 | soft-ce，選項順序每題隨機打亂 |
| 序列長度 | max_len 768、head_max_len 512 |
| 學習率 | 編碼器 2.5e-5、輸出頭 1e-4，餘弦衰減到 1e-6；weight decay 0.01、梯度裁剪 1.0 |
| 批次與輪數 | 8 題 × 累積 8 次，4 輪 |
| 硬體與時間 | 一張 RTX 5060 Ti 16 GB，約 2 小時 |
| 種子 | 0，只訓練一次 |

**ONNX**（`training/prune_laya_vocab.py`）：只保留特殊與位元組詞元、全部由中日韓字元組成的詞元，以及訓練語料與詞庫實際用到的詞元；再以 INT8 動態量化 MatMul 與嵌入。

## 評測

流程同輸入法：注音 → 詞庫前 16 名候選整句 → 排序器重排，與詞庫分數混合（權重 1.0），並對大陸寫法（身份、計劃…）每處扣 4 分。整句正確率包含詞庫前 16 名沒有正解的句子。以下為本 repo 的 INT8 ONNX、CPU（ONNX Runtime 8 執行緒）。

| 評測集 | 內容 |
|---|---|
| Common Voice dev（1,000）／test（2,000） | Common Voice zh-TW 句子切成 4–24 字的片段 |
| 立法院（1,000）、新聞稿（1,000） | 從同來源留出、不進訓練的片段 |
| 維基（2,000） | 維基百科句子，**只用來和舊版比較**，不隨模型公開 |
| 台灣用字（24） | 同音但兩岸寫法不同的句子；只當參考 |

| 選字 | CV dev | CV test | 立法院 | 新聞稿 | 維基 | 台灣用字 |
|---|---|---|---|---|---|---|
| 只用詞庫 | 71.4% | 71.2% | 69.3% | 56.7% | 52.4% | 83.3% |
| 舊版（含維基百科資料，已不使用） | 84.9% | 82.3% | 82.0% | 70.6% | 69.3% | 54.2% |
| **本模型** | **85.2%** | **83.0%** | **86.5%** | **76.1%** | 66.5% | **75.0%** |
| 〔對照〕Qwen3.5-2B-TW-IME（需要顯示卡） | 87.7% | 86.1% | 89.3% | 78.5% | 73.0% | 95.8% |

只用排序器（詞庫權重 0）：dev 84.7%、test 82.6%、立法院 86.2%、新聞稿 75.4%、維基 66.2%、台灣用字 70.8%。

延遲（每次選字、16 個候選的中位數）：INT8 在 i5-12400（8 執行緒）上 Common Voice 片段約 50 ms、立法院約 68 ms、新聞稿約 83 ms；fp16 在 RTX 5060 Ti（ONNX Runtime 1.30 CUDA）上 Common Voice 片段約 11 ms。INT8 放到 GPU 上不會變快（多數 INT8 節點沒有 CUDA 實作，留在 CPU）。

fp16 版（GPU）在 Common Voice dev 上為 86.1%（INT8 為 85.2%），量化前的 fp32 也是 86.1%。

### 限制

- 只訓練一次、沒有重複種子，沒有信賴區間；1,000 句的集合 1 點約等於 10 句。
- 立法院與新聞稿測試集和訓練資料同來源（不同句子），這兩組的進步可能高估在其他文體上的效果。
- 維基比舊版低 2.8 點，因為這次沒有維基資料；百科式、專有名詞多的文字表現較差。
- 台灣用字比只用詞庫還差（75.0% 對 83.3%）：同音的兩岸寫法仍要靠扣分表補救。
- 模型學到的是「在這份詞庫的錯誤分布下挑出正確的」；評測用的注音由詞庫自動轉出，不是人工標注。
- 生成句來自另一個模型，已濾掉簡體字、一份大陸用詞表與重複句，但沒有人工逐句檢查。

## 授權

- 本模型以 **Apache-2.0** 釋出，與基底 Laya 相同（Laya 的 repo 沒有附 LICENSE 檔，授權標在模型卡的中繼資料）；編碼器 mmBERT-base 為 MIT。
- 訓練資料：Common Voice（CC0）；立法院、行政院、法務部的開放資料依政府資料開放授權條款－第1版使用，出處標示見 `DATA.md`；Breeze-7B-Instruct 生成的句子沒有使用限制，隨本模型公開；題目選項來自 McBopomofo（MIT）與新酷音詞庫（LGPL-2.1-or-later）的詞條。

## English summary

A fine-tune of `convaiinnovations/laya-multilingual` (mmBERT-base, non-autoregressive) that ranks homophone candidate sentences for a Bopomofo (Zhuyin) input method in one forward pass. Every training source may be reused freely: Common Voice zh-TW (CC0), Legislative Yuan gazette speeches and Executive Yuan news releases (Taiwan Open Government Data License v1.0, CC BY 4.0-compatible), laws (not copyrightable) and everyday sentences generated locally with Breeze-7B-Instruct (Apache-2.0); options come from the McBopomofo (MIT) and libchewing (LGPL-2.1) lexicons. No Wikipedia or Ministry of Education dictionary data. 102,581 auto-generated cases, 4 epochs, single run. With the shipped INT8 ONNX graph (162 MB, ~50 ms on 8 CPU threads) and the IME's mixing: 85.2 / 83.0% (Common Voice dev / test), 86.5% (Legislative Yuan), 76.1% (news), 66.5% (Wikipedia). See `DATA.md` for sources and attribution.
