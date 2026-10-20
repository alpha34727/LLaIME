"""Sentences from the Legislative Yuan Gazette's meeting records: spoken Taiwan Mandarin.

Source: 立法院公報 (院會 and 委員會 records), published as open data by the Legislative Yuan
under the Open Government Data License, version 1.0 (政府資料開放授權條款－第1版,
https://data.gov.tw/license, compatible with CC BY 4.0; dataset 公報原始檔案,
https://data.ly.gov.tw/getds.action?id=41). The speeches are read through g0v's LY API
(https://v2.ly.govapi.tw), which serves the same gazette files split into speaker turns.

Speaker labels (主席：, 李部長洋：), stage directions in parentheses and non-speech blocks are
dropped; the rest is split into sentences at 。！？；. Output: one sentence per line, and a
provenance TSV (record id, date, official .doc URL, sentences taken) next to it.

  uv run python fetch_ly.py ../data/raw/free/ly --records 400
"""

import argparse
import json
import random
import re
import time
import urllib.request
from pathlib import Path

API = "https://v2.ly.govapi.tw"
# 類別代碼: 1 院會, 3 委員會 (the records with speeches).
KINDS = {1, 3}
SPEAKER = re.compile(r"^[^：:\s]{1,24}[：:]")
PAREN = re.compile(r"（[^（）]*）|\([^()]*\)")
SENTENCE_END = re.compile(r"(?<=[。！？；])")
HAN = re.compile(r"[一-鿿]")


def get(path: str) -> dict:
    request = urllib.request.Request(API + path, headers={"User-Agent": "LLaIME-data/1.0"})
    for attempt in range(4):
        try:
            with urllib.request.urlopen(request, timeout=60) as response:
                return json.load(response)
        except Exception:  # noqa: BLE001 - retried, then raised
            if attempt == 3:
                raise
            time.sleep(5 * (attempt + 1))
    raise AssertionError


def speech_sentences(parsed: dict):
    for block in parsed.get("blocks", []):
        if not block or not SPEAKER.match(block[0]):
            continue  # agenda text, tables, written statements
        # A block can hold several turns: every line may start with a speaker label.
        lines = [SPEAKER.sub("", line, count=1) for line in block]
        for line in lines:
            line = PAREN.sub("", line).strip()
            for sentence in SENTENCE_END.split(line):
                sentence = sentence.strip()
                if len(HAN.findall(sentence)) >= 4:
                    yield sentence


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("output", type=Path, help="directory")
    parser.add_argument("--records", type=int, default=400, help="meeting records to read")
    parser.add_argument("--max-per-record", type=int, default=400,
                        help="sentences taken from one record, so no meeting dominates")
    parser.add_argument("--pages", type=int, default=300, help="gazette list pages to sample from")
    parser.add_argument("--seed", type=int, default=20261020)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    rng = random.Random(args.seed)

    gazettes = []
    for page in range(1, args.pages + 1):
        data = get(f"/gazettes?limit=10&page={page}")
        gazettes += [g["公報編號"] for g in data["gazettes"]]
        if page >= data["total_page"]:
            break
    rng.shuffle(gazettes)

    seen_docs, sentences, provenance = set(), [], []
    seen_sentences = set()
    for gazette in gazettes:
        if len(provenance) >= args.records:
            break
        agendas = get(f"/gazette/{gazette}/agendas?limit=100").get("gazetteagendas", [])
        for agenda in agendas:
            if agenda.get("類別代碼") not in KINDS or len(provenance) >= args.records:
                continue
            parsed_url = next((u["url"] for u in agenda.get("處理後公報網址", [])
                               if u["type"] == "parsed"), None)
            if not parsed_url or parsed_url in seen_docs:
                continue
            seen_docs.add(parsed_url)
            try:
                parsed = get(parsed_url.removeprefix(API))
            except Exception as e:  # noqa: BLE001 - a missing record is skipped
                print(f"skip {parsed_url}: {e}")
                continue
            taken = [s for s in speech_sentences(parsed) if s not in seen_sentences]
            rng.shuffle(taken)
            taken = taken[: args.max_per_record]
            seen_sentences.update(taken)
            sentences += taken
            doc = (agenda.get("doc檔案下載位置") or [""])[0]
            provenance.append((agenda["公報議程編號"], parsed.get("date", ""), doc, len(taken)))
            print(f"{len(provenance)}/{args.records} {agenda['公報議程編號']} {len(taken)}")
            time.sleep(0.5)

    (args.output / "sentences.txt").write_text("\n".join(sentences) + "\n", encoding="utf-8")
    with (args.output / "provenance.tsv").open("w", encoding="utf-8", newline="\n") as f:
        f.write("record\tdate\tofficial_doc\tsentences\n")
        for row in provenance:
            f.write("\t".join(str(x) for x in row) + "\n")
    print(f"{len(sentences)} sentences from {len(provenance)} records")


if __name__ == "__main__":
    main()
