"""Write the training-data manifest (data/sources.json) and its human-readable form (DATA.md).

Lists every input of the Laya and Qwen3.5-2B-TW-IME training runs: where it came from, under
which terms, when it was downloaded, the SHA-256 of what was used, and how it was used. Fed
from the provenance files the fetch scripts leave next to their output. Nothing here goes into
the models; it is published beside them.

  uv run python data_manifest.py --out-json ../data/sources.json --out-md ../docs/huggingface/DATA.md
"""

import argparse
import csv
import hashlib
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
RAW = ROOT / "data/raw"
OGDL = "政府資料開放授權條款－第1版 (Open Government Data License, version 1.0), compatible with CC BY 4.0"
OGDL_URL = "https://data.gov.tw/license"


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 24), b""):
            h.update(chunk)
    return h.hexdigest()


def lines(path: Path) -> int:
    return sum(1 for _ in path.open(encoding="utf-8"))


def tsv(path: Path) -> list[dict]:
    with path.open(encoding="utf-8") as f:
        return list(csv.DictReader(f, delimiter="\t"))


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--out-json", type=Path, default=ROOT / "data/sources.json")
    parser.add_argument("--out-md", type=Path, default=ROOT / "docs/huggingface/DATA.md")
    args = parser.parse_args()

    cv = [RAW / "commonvoice" / f for f in ("sentence-collector.txt", "setences.txt", "taipei_city_gov.txt")]
    ly = RAW / "free/ly"
    press = RAW / "free/press"
    laws = RAW / "free/laws"
    synthetic = RAW / "free/synthetic"
    synthetic_info = json.loads((synthetic / "provenance.json").read_text(encoding="utf-8"))
    ly_records = tsv(ly / "provenance.tsv")
    sources = [
        {
            "id": "commonvoice-zh-TW",
            "name": "Mozilla Common Voice zh-TW sentence collection",
            "url": "https://github.com/common-voice/common-voice/tree/main/server/data/zh-TW",
            "license": "CC0-1.0", "license_url": "https://creativecommons.org/publicdomain/zero/1.0/",
            "files": [{"name": p.name, "sha256": sha256(p), "lines": lines(p)} for p in cv],
            "use": "training text (Laya, Qwen); evaluation sets commonvoice_dev/test",
        },
        {
            "id": "ly-gazette",
            "name": "立法院公報 meeting records (院會、委員會), speech turns",
            "provider": "立法院", "dataset": "公報原始檔案 (https://data.ly.gov.tw/getds.action?id=41)",
            "via": "g0v LY API https://v2.ly.govapi.tw (same gazette files split into speaker turns)",
            "license": OGDL, "license_url": OGDL_URL,
            "attribution": "立法院，公報原始檔案，政府資料開放授權條款－第1版",
            "records": len(ly_records),
            "dates": [min(r["date"] for r in ly_records if r["date"]),
                      max(r["date"] for r in ly_records if r["date"])],
            "files": [{"name": "sentences.txt", "sha256": sha256(ly / "sentences.txt"),
                       "lines": lines(ly / "sentences.txt")},
                      {"name": "provenance.tsv (official .doc URL of every record)",
                       "sha256": sha256(ly / "provenance.tsv")}],
            "use": "training text (Laya, Qwen); evaluation set ly_test",
        },
        {
            "id": "ey-news",
            "name": "行政院即時新聞、行政院雲嘉南區聯合服務中心焦點新聞",
            "provider": "行政院",
            "url": ["https://www.ey.gov.tw/opendata/BF42D887A7B414E2/154",
                    "https://www.ey.gov.tw/opendata/BF42D887A7B414E2/88"],
            "license": OGDL, "license_url": OGDL_URL,
            "attribution": "行政院，行政院即時新聞／雲嘉南區聯合服務中心焦點新聞，政府資料開放授權條款－第1版",
            "downloads": tsv(press / "provenance.tsv"),
            "files": [{"name": "sentences.txt", "sha256": sha256(press / "sentences.txt"),
                       "lines": lines(press / "sentences.txt")}],
            "use": "training text (Laya, Qwen); evaluation set press_test",
        },
        {
            "id": "moj-laws",
            "name": "全國法規資料庫 laws in force (articles)",
            "provider": "法務部", "url": "https://law.moj.gov.tw/api/Ch/Law/json",
            "attribution": "法務部，全國法規資料庫，政府資料開放授權條款－第1版",
            "license": "Not subject to copyright (著作權法第9條第1項第1款); published under " + OGDL,
            "license_url": OGDL_URL,
            "downloads": tsv(laws / "provenance.tsv"),
            "files": [{"name": "sentences.txt", "sha256": sha256(laws / "sentences.txt"),
                       "lines": lines(laws / "sentences.txt")}],
            "use": "training text (Laya, Qwen), a small share",
        },
        {
            "id": "synthetic-breeze",
            "name": "Everyday sentences generated locally by Breeze-7B-Instruct-v1_0",
            "generator": synthetic_info["generator"],
            "generator_url": "https://huggingface.co/MediaTek-Research/Breeze-7B-Instruct-v1_0",
            "license": "Model Apache-2.0, no terms on its output; the generated text is released "
                       "with the models",
            "model_file_sha256": synthetic_info["model_sha256"],
            "prompt": synthetic_info["prompt"], "grid": [len(synthetic_info["topics"]),
                                                         len(synthetic_info["situations"])],
            "kept": synthetic_info["kept"], "rejected": synthetic_info["rejected"],
            "files": [{"name": "sentences.txt", "sha256": sha256(synthetic / "sentences.txt"),
                       "lines": lines(synthetic / "sentences.txt")}],
            "use": "training text (Laya, Qwen)",
        },
        {
            "id": "mcbopomofo",
            "name": "McBopomofo phrase table (readings, scores)",
            "url": "https://github.com/openvanilla/McBopomofo", "license": "MIT",
            "files": [{"name": "data.txt", "sha256": sha256(RAW / "McBopomofo/Source/Data/data.txt")}],
            "use": "readings of training/evaluation text; candidate sentences (lexicon N best)",
        },
        {
            "id": "libchewing-data",
            "name": "libchewing-data tsi.csv (phrases missing from McBopomofo)",
            "url": "https://github.com/chewing/libchewing-data", "license": "LGPL-2.1-or-later",
            "files": [{"name": "supplement-chewing.txt (derived by tools/supplement_lexicon.py)",
                       "sha256": sha256(ROOT / "data/build/supplement-chewing.txt")}],
            "use": "readings; candidate sentences (lexicon N best)",
        },
    ]
    not_used = [
        "教育部《重編國語辭典修訂本》 (CC BY-ND 3.0 TW): no headword, reading or text from it",
        "Wikipedia (CC BY-SA): no text from it in training; wikipedia_test.tsv is used only to "
        "compare with the earlier models and is not published",
    ]
    manifest = {"sources": sources, "not_used": not_used}
    args.out_json.write_text(json.dumps(manifest, ensure_ascii=False, indent=2), encoding="utf-8")

    md = ["# 訓練資料來源", "",
          "Laya（laya-multilingual-TW-IME）與 Qwen3.5-2B-TW-IME 的所有訓練輸入。機器可讀版本與各檔案的 SHA-256 見 `sources.json`。", ""]
    md += ["| 來源 | 授權 | 用途 |", "|---|---|---|"]
    for s in sources:
        md.append(f"| {s['name']} | {s['license']} | {s['use']} |")
    md += ["", "## 出處標示（政府資料開放授權條款）", ""]
    md += [f"- {s['attribution']}（{OGDL_URL}）" for s in sources if "attribution" in s]
    md += ["", "## 未使用", ""] + [f"- {x}" for x in not_used]
    args.out_md.write_text("\n".join(md) + "\n", encoding="utf-8", newline="\n")
    print(f"wrote {args.out_json} and {args.out_md}")


if __name__ == "__main__":
    main()
