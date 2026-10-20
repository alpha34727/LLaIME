"""Sentences from Executive Yuan news releases: present-day written Taiwan Mandarin.

Sources (both under the Open Government Data License, version 1.0, 政府資料開放授權條款－
第1版, https://data.gov.tw/license, compatible with CC BY 4.0):
  * 行政院即時新聞 (https://www.ey.gov.tw/opendata/BF42D887A7B414E2/154)
  * 行政院雲嘉南區聯合服務中心－焦點新聞 (https://www.ey.gov.tw/opendata/BF42D887A7B414E2/88)

The JSON feeds are saved as downloaded (raw/) with their SHA-256 in provenance.tsv; the
article bodies are split into sentences at 。！？；. Output: one sentence per line.

  uv run python fetch_press.py ../data/raw/free/press
"""

import argparse
import hashlib
import json
import re
import urllib.request
from datetime import date
from pathlib import Path

FEEDS = {
    "ey-news": "https://www.ey.gov.tw/NewOpenData/json/154",
    "ey-yunchianan": "https://www.ey.gov.tw/NewOpenData/json/88",
}
SENTENCE_END = re.compile(r"(?<=[。！？；])")
HAN = re.compile(r"[一-鿿]")
TAGS = re.compile(r"<[^>]+>")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("output", type=Path, help="directory")
    args = parser.parse_args()
    raw = args.output / "raw"
    raw.mkdir(parents=True, exist_ok=True)

    sentences, seen, provenance = [], set(), []
    for name, url in FEEDS.items():
        request = urllib.request.Request(url, headers={"User-Agent": "Mozilla/5.0"})
        with urllib.request.urlopen(request, timeout=120) as response:
            body = response.read()
        (raw / f"{name}.json").write_bytes(body)
        articles = json.loads(body.decode("utf-8-sig"))
        count = 0
        for article in articles:
            text = TAGS.sub("", article.get("內容", ""))
            for line in text.splitlines():
                for sentence in SENTENCE_END.split(line):
                    sentence = sentence.strip()
                    if len(HAN.findall(sentence)) >= 4 and sentence not in seen:
                        seen.add(sentence)
                        sentences.append(sentence)
                        count += 1
        provenance.append((name, url, date.today().isoformat(), hashlib.sha256(body).hexdigest(),
                           len(articles), count))
        print(f"{name}: {len(articles)} articles, {count} sentences")

    (args.output / "sentences.txt").write_text("\n".join(sentences) + "\n", encoding="utf-8")
    with (args.output / "provenance.tsv").open("w", encoding="utf-8", newline="\n") as f:
        f.write("feed\turl\tdownloaded\tsha256\tarticles\tsentences\n")
        for row in provenance:
            f.write("\t".join(str(x) for x in row) + "\n")


if __name__ == "__main__":
    main()
