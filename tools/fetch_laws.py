"""Sentences from the articles of Taiwan's laws (全國法規資料庫), a small share of formal text.

Laws are not subject to copyright (著作權法第9條第1項第1款), and the Ministry of Justice
publishes the database as open data (https://law.moj.gov.tw/api/Ch/Law/json, Open Government
Data License, version 1.0). Only laws currently in force (not 廢止) are used; article text is
split into sentences at 。；：, and each law contributes at most --max-per-law sentences so the
long codes do not dominate. Output: one sentence per line.

  uv run python fetch_laws.py ../data/raw/free/laws
"""

import argparse
import hashlib
import io
import json
import random
import re
import urllib.request
import zipfile
from datetime import date
from pathlib import Path

URL = "https://law.moj.gov.tw/api/Ch/Law/json"
SENTENCE_END = re.compile(r"(?<=[。；：])")
HAN = re.compile(r"[一-鿿]")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("output", type=Path, help="directory")
    parser.add_argument("--max-per-law", type=int, default=20)
    parser.add_argument("--seed", type=int, default=20261020)
    args = parser.parse_args()
    raw = args.output / "raw"
    raw.mkdir(parents=True, exist_ok=True)
    rng = random.Random(args.seed)

    request = urllib.request.Request(URL, headers={"User-Agent": "Mozilla/5.0"})
    with urllib.request.urlopen(request, timeout=300) as response:
        body = response.read()
    (raw / "ChLaw.json.zip").write_bytes(body)
    with zipfile.ZipFile(io.BytesIO(body)) as z:
        data = json.loads(z.read("ChLaw.json").decode("utf-8-sig"))

    sentences, seen, laws = [], set(), 0
    for law in data["Laws"]:
        if law.get("LawAbandonNote"):
            continue
        taken = []
        for article in law.get("LawArticles", []):
            if article.get("ArticleType") != "A":
                continue
            for line in article.get("ArticleContent", "").splitlines():
                line = re.sub(r"^[一二三四五六七八九十]+、|^（[一二三四五六七八九十]+）", "", line.strip())
                for sentence in SENTENCE_END.split(line):
                    sentence = sentence.strip()
                    if len(HAN.findall(sentence)) >= 4 and sentence not in seen:
                        seen.add(sentence)
                        taken.append(sentence)
        rng.shuffle(taken)
        sentences += taken[: args.max_per_law]
        laws += 1

    (args.output / "sentences.txt").write_text("\n".join(sentences) + "\n", encoding="utf-8")
    (args.output / "provenance.tsv").write_text(
        "url\tdownloaded\tsha256\tlaws\tsentences\n"
        f"{URL}\t{date.today().isoformat()}\t{hashlib.sha256(body).hexdigest()}\t{laws}\t{len(sentences)}\n",
        encoding="utf-8")
    print(f"{len(sentences)} sentences from {laws} laws")


if __name__ == "__main__":
    main()
