# 訓練資料來源

Laya（laya-multilingual-TW-IME）與 Qwen3.5-2B-TW-IME 的所有訓練輸入。機器可讀版本與各檔案的 SHA-256 見 `sources.json`。

| 來源 | 授權 | 用途 |
|---|---|---|
| Mozilla Common Voice zh-TW sentence collection | CC0-1.0 | training text (Laya, Qwen); evaluation sets commonvoice_dev/test |
| 立法院公報 meeting records (院會、委員會), speech turns | 政府資料開放授權條款－第1版 (Open Government Data License, version 1.0), compatible with CC BY 4.0 | training text (Laya, Qwen); evaluation set ly_test |
| 行政院即時新聞、行政院雲嘉南區聯合服務中心焦點新聞 | 政府資料開放授權條款－第1版 (Open Government Data License, version 1.0), compatible with CC BY 4.0 | training text (Laya, Qwen); evaluation set press_test |
| 全國法規資料庫 laws in force (articles) | Not subject to copyright (著作權法第9條第1項第1款); published under 政府資料開放授權條款－第1版 (Open Government Data License, version 1.0), compatible with CC BY 4.0 | training text (Laya, Qwen), a small share |
| Everyday sentences generated locally by Breeze-7B-Instruct-v1_0 | Model Apache-2.0, no terms on its output; the generated text is released with the models | training text (Laya, Qwen) |
| McBopomofo phrase table (readings, scores) | MIT | readings of training/evaluation text; candidate sentences (lexicon N best) |
| libchewing-data tsi.csv (phrases missing from McBopomofo) | LGPL-2.1-or-later | readings; candidate sentences (lexicon N best) |

## 出處標示（政府資料開放授權條款）

- 立法院，公報原始檔案，政府資料開放授權條款－第1版（https://data.gov.tw/license）
- 行政院，行政院即時新聞／雲嘉南區聯合服務中心焦點新聞，政府資料開放授權條款－第1版（https://data.gov.tw/license）
- 法務部，全國法規資料庫，政府資料開放授權條款－第1版（https://data.gov.tw/license）

## 未使用

- 教育部《重編國語辭典修訂本》 (CC BY-ND 3.0 TW): no headword, reading or text from it
- Wikipedia (CC BY-SA): no text from it in training; wikipedia_test.tsv is used only to compare with the earlier models and is not published
