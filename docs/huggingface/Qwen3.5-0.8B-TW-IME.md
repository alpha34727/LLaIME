---
language:
- zh
license: apache-2.0
base_model: Qwen/Qwen3.5-0.8B
library_name: transformers
pipeline_tag: text-generation
tags:
- qwen3_5
- lora
- traditional-chinese
- zh-tw
- taiwan
- input-method
- bopomofo
- gguf
---

# Qwen3.5-0.8B-TW-IME

[Qwen/Qwen3.5-0.8B](https://huggingface.co/Qwen/Qwen3.5-0.8B) 以台灣正體中文語料做 LoRA 持續預訓練後合併的版本，用途是**替注音輸入法的候選句打分數**（計算 log P(候選｜上文)），不是聊天模型。

> **先讀這幾點**
>
> - 只在「注音選字」這一個任務上評測過（見〈評測〉）。**沒有**評測對話、指令遵循、推理、程式或視覺能力；訓練只用純文字，這些能力可能比原版差。
> - **訓練資料全部可以自由再利用**：CC0、政府資料開放授權條款（與 CC BY 4.0 相容）、不受著作權保護的法規，以及由 Apache-2.0 模型在本機生成的句子。沒有使用維基百科與教育部辭典。來源、授權與雜湊值見 `DATA.md` 與 `sources.json`。
> - 相對原版：Common Voice +1.8～1.9、立法院 +2.5、新聞稿 +1.2、台灣用字 +4.2 個百分點，維基 −0.3（單次訓練、無信賴區間）。整體約等於未微調的 2B 原版。
> - 比同一專案的 [Qwen3.5-2B-TW-IME](https://huggingface.co/REPO_OWNER/Qwen3.5-2B-TW-IME) 低約 1 個百分點；在顯示卡上延遲差不多，**在 CPU 上約快一倍**（每次選字約 0.5 秒對 0.95 秒）。
> - 沒有附 `mtp.*`（多 token 預測）張量。

## 用途

[LLaIME](PROJECT_URL)（Windows 11 注音輸入法）把打出的注音經詞庫轉成前 16 名候選句，再由本機語言模型算每一句的 log P，挑最通順的。候選一定符合所打的讀音，模型只負責排序，不會自由生成。訓練格式與輸入法送給模型的完全相同：

```
以下是一段台灣繁體中文的文字：\n{上文}{候選句}
```

訓練時損失只算正文（提示句不算），所以模型學到的正是輸入法要用的量。

## 使用方式

計算 log P(文字｜提示＋上文)（單位 nat，越大越通順）：

```python
import torch
from transformers import AutoModelForCausalLM, AutoTokenizer

path = "REPO_ID"
PROMPT = "以下是一段台灣繁體中文的文字：\n"
tok = AutoTokenizer.from_pretrained(path)
model = AutoModelForCausalLM.from_pretrained(path, dtype=torch.bfloat16).cuda().eval()

@torch.no_grad()
def log_prob(context: str, text: str) -> float:
    prefix = tok(PROMPT + context, add_special_tokens=False)["input_ids"]
    target = tok(text, add_special_tokens=False)["input_ids"]
    ids = torch.tensor([prefix + target]).cuda()
    logits = model(input_ids=ids).logits[0, len(prefix) - 1 : -1].float()
    logp = torch.log_softmax(logits, dim=-1)
    return logp[torch.arange(len(target)), torch.tensor(target)].sum().item()

print(log_prob("請出示您的", "身分證"), log_prob("請出示您的", "身份證"))
```

## 檔案

| 檔案 | 說明 |
|---|---|
| `model.safetensors` 與設定檔 | 合併後的 bf16 權重，架構 `Qwen3_5ForConditionalGeneration`，視覺部分與原版相同 |
| `Qwen3.5-0.8B-TW-IME-Q4_K_M.gguf` | llama.cpp 用的 Q4_K_M 量化，輸入法實際使用的版本 |
| `DATA.md`、`sources.json` | 每個訓練來源的出處、授權、下載日期與 SHA-256 |
| `training/` | 資料下載、清理、生成與訓練腳本，生成的句子（`synthetic_sentences.txt`）與訓練記錄 |

GGUF 以 llama.cpp 的 `convert_hf_to_gguf.py --outtype bf16` 轉檔，再用 `llama-quantize … Q4_K_M` 量化。

## 訓練

**基底**：[Qwen/Qwen3.5-0.8B](https://huggingface.co/Qwen/Qwen3.5-0.8B)（後訓練版）。語料、處理與訓練設定與 [Qwen3.5-2B-TW-IME](https://huggingface.co/REPO_OWNER/Qwen3.5-2B-TW-IME) 完全相同，只換基底模型。

**語料**（`training/build_lm_corpus.py`）共 188,098 段（約 710 萬字），500 段留作驗證，187,598 段（4,688,097 token）用於訓練：

| 來源 | 內容 | 授權 |
|---|---|---|
| Common Voice zh-TW 句子 | 3 個句子檔 | CC0 |
| 立法院公報 | 400 場院會與委員會紀錄的發言（去掉發言人與括號註記），13.6 萬句 | 政府資料開放授權條款－第1版 |
| 行政院即時新聞、雲嘉南區聯合服務中心焦點新聞 | 各 500 則，1.2 萬句 | 政府資料開放授權條款－第1版 |
| 全國法規資料庫 | 現行法規條文，抽樣 5,000 句 | 不受著作權保護（著作權法第 9 條） |
| 本機生成的日常口語句 | [Breeze-7B-Instruct-v1_0](https://huggingface.co/MediaTek-Research/Breeze-7B-Instruct-v1_0)（Apache-2.0）依「主題 × 情境」生成 3 萬句，濾掉簡體字、大陸用詞、重複句與評測句 | 隨本模型公開 |

評測句與其所在的整句全部排除；漢字比例低於 60% 的文字丟棄；樣板句每種最多 2 句；洗牌（seed 20261015）。

**方法**（`training/lora_qwen.py`）：

| 項目 | 設定 |
|---|---|
| LoRA | r = 16、alpha = 32、dropout = 0.05；語言模型的所有線性層（注意力、線性注意力、MLP），可訓練參數 10,822,656（1.25%）。視覺塔與 LM head 不動 |
| 輸入 | 提示句＋正文，最多 256 token；損失只算正文 |
| 最佳化 | AdamW、lr 1e-4、5% 暖機後餘弦衰減、梯度裁剪 1.0 |
| 批次 | 等效批次 16、1 輪共 11,725 步 |
| 硬體與時間 | 一張 RTX 5060 Ti 16 GB，約 3 小時，顯存峰值 7.6 GB |
| 種子 | 20261015，只訓練一次 |

**驗證損失**（500 段留出的語料，每 token 的 nat）：5.063 → 3.869。

## 評測

輸入法的真實流程：注音 → 詞庫（McBopomofo＋新酷音）前 16 名候選整句 → 語言模型重排。

> 混合分數 ＝ LM 分數 ＋ 0.75 × 詞庫分數 − 4 × 非台灣標準寫法的字詞數

整句正確率，包含詞庫前 16 名沒有正解的句子（一定算錯）。全部用 Q4_K_M、同一套轉檔流程。

| 評測集 | 內容 |
|---|---|
| Common Voice dev（1,000）／test（2,000） | Common Voice zh-TW 句子切成 4–24 字的片段；混合權重在 dev 上選定 |
| 立法院（1,000） | 從立法院發言留出的片段，不進訓練 |
| 新聞稿（1,000） | 從行政院新聞稿留出的片段，不進訓練 |
| 維基（2,000） | 維基百科句子，**只用來和舊版比較**，不隨模型公開 |
| 台灣用字（24） | 同音但兩岸寫法不同的句子；用字表是看過錯誤後才寫的，只當參考 |

| 選字 | CV dev | CV test | 立法院 | 新聞稿 | 維基 | 台灣用字 |
|---|---|---|---|---|---|---|
| 只用詞庫 | 71.4% | 71.2% | 69.3% | 56.7% | 52.4% | 83.3% |
| 詞庫前 16 名含正解（上限） | 93.8% | 92.9% | 92.7% | 82.0% | 77.0% | 95.8% |
| Qwen3.5-0.8B 原版（同流程轉檔、量化） | 85.2% | 83.2% | 85.3% | 76.8% | 72.0% | 87.5% |
| **本模型** | **87.1%** | **85.0%** | **87.8%** | **78.0%** | 71.7% | **91.7%** |
| 〔對照〕Qwen3.5-2B 原版 | 87.5% | 85.5% | 86.8% | 77.7% | 73.7% | 91.7% |
| 〔對照〕Qwen3.5-2B-TW-IME | 87.7% | 86.1% | 89.3% | 78.5% | 73.0% | 95.8% |

只看語言模型（詞庫權重 0，仍有台灣用字扣分）：原版 → 本模型 dev 82.9 → 86.2%、test 82.4 → 84.0%、立法院 84.4 → 87.6%、新聞稿 75.9 → 78.4%、維基 71.8 → 71.3%、台灣用字 91.7 → 91.7%。

延遲（每次選字、16 個候選的中位數）：RTX 5060 Ti 上約 48 ms（Common Voice 片段）到 75 ms（新聞稿）；i5-12400 CPU 上約 490 ms（Common Voice dev 前 150 句；2B 為 950 ms）。

### 限制

- 只訓練一次、沒有重複種子，沒有信賴區間；1,000 句的集合 1 點約等於 10 句。
- 立法院與新聞稿測試集和訓練語料同來源（不同句子），這兩組的進步可能高估模型在其他文體上的效果。
- 維基沒有進步（−0.3），因為語料沒有維基；百科式、專有名詞多的文字表現較差。
- 評測用的注音由詞庫自動轉出，不是人工標注。
- 生成句來自另一個模型，可能帶有它的用詞偏好；已濾掉簡體字與一份大陸用詞表，但沒有人工逐句檢查。

## 授權

- 權重以 **Apache-2.0** 釋出，與基底模型相同（基底 © Qwen Team）。
- 訓練資料：Common Voice（CC0）；立法院、行政院、法務部的開放資料依政府資料開放授權條款－第1版使用，出處標示見 `DATA.md`；Breeze-7B-Instruct（Apache-2.0）生成的句子沒有使用限制，隨本模型公開。
- 候選句與注音來自 McBopomofo（MIT）與新酷音詞庫（LGPL-2.1-or-later），只在評測時使用，不在訓練語料內。

## English summary

A LoRA (r=16, all language-model linear layers) continued-pretraining of Qwen/Qwen3.5-0.8B on the same 4.7M tokens of freely reusable Taiwan Mandarin as Qwen3.5-2B-TW-IME: Common Voice zh-TW (CC0), Legislative Yuan gazette speeches and Executive Yuan news releases (Taiwan Open Government Data License v1.0, CC BY 4.0-compatible), laws (not copyrightable), and 30k everyday sentences generated locally with Breeze-7B-Instruct (Apache-2.0). No Wikipedia or Ministry of Education dictionary data. It scores candidate sentences for a Bopomofo input method; it is not a chat model. Sentence accuracy with the IME's mixing: 87.1 / 85.0% (Common Voice dev / test), 87.8% (Legislative Yuan), 78.0% (news), 71.7% (Wikipedia), vs. 85.2 / 83.2 / 85.3 / 76.8 / 72.0% for the stock 0.8B model, about as good as the stock 2B model and twice as fast on a CPU; single run, no confidence intervals.
