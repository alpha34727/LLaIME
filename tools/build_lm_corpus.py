"""Taiwan Traditional Chinese text for LoRA continued pre-training of the ranking LM (stage 4b).

Sources: zetavg/zh-tw-wikipedia shards (paragraphs) and Common Voice zh-TW sentences. Every
paragraph or sentence containing an evaluation segment (tests/eval/*.tsv), or one of the
held-out Wikipedia test sentences, is left out. Long paragraphs are cut at sentence ends into
chunks of at most --max-chars characters. Mostly-ASCII text (formulas, code) is skipped, and
templated text (小行星 2632 以…命名) is kept at most twice per template, numbers ignored.
Output: JSONL {"text": ...}, shuffled.
"""

import argparse
import json
import random
import re
import sys
from pathlib import Path

import pyarrow.parquet as pq

sys.path.insert(0, str(Path(__file__).resolve().parent))
from extract_wiki_text import LINK, MARKUP  # noqa: E402

SENTENCE_END = re.compile(r"(?<=[。！？；])")
HAN = re.compile(r"[㐀-䶿一-鿿]")
NUMBER = re.compile(r"[0-9０-９.,]+")
MAX_PER_TEMPLATE = 2


def paragraphs(markdown: str):
    for line in markdown.splitlines():
        line = line.strip()
        if not line or line[0] in "#*-|>!<" or line[0].isdigit():
            continue
        line = MARKUP.sub("", LINK.sub(r"\1", line))
        if len(HAN.findall(line)) >= 10:
            yield line


def chunks(text: str, max_chars: int):
    cur = ""
    for s in SENTENCE_END.split(text):
        if cur and len(cur) + len(s) > max_chars:
            yield cur
            cur = ""
        cur += s
    if cur:
        yield cur[:max_chars]


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("output", type=Path)
    parser.add_argument("--wiki", type=Path, nargs="*", default=[])
    parser.add_argument("--sentences", type=Path, nargs="*", default=[])
    parser.add_argument("--exclude", type=Path, nargs="*", default=[],
                        help="eval TSVs (column 3 is the segment) or text files of sentences")
    parser.add_argument("--max-chars", type=int, default=200)
    parser.add_argument("--seed", type=int, default=20261015)
    args = parser.parse_args()

    held_out = set()
    for path in args.exclude:
        for line in path.read_text(encoding="utf-8").splitlines():
            cols = line.split("\t")
            held_out.add(cols[2] if path.suffix == ".tsv" and len(cols) >= 3 else line.strip())
    held_out.discard("")
    # Look held-out strings up by a 4-character key, so each text is checked against only the
    # few that could occur in it.
    by_key: dict[str, list[str]] = {}
    for h in held_out:
        by_key.setdefault(h[:4], []).append(h)

    def clean(text: str) -> bool:
        for i in range(len(text) - 3):
            for h in by_key.get(text[i:i + 4], ()):
                if text.startswith(h, i):
                    return False
        return True

    texts, dropped = [], 0
    for shard in args.wiki:
        for markdown in pq.read_table(shard, columns=["markdown"]).column("markdown").to_pylist():
            for para in paragraphs(markdown or ""):
                if clean(para):
                    texts.extend(chunks(para, args.max_chars))
                else:
                    dropped += 1
    for path in args.sentences:
        for line in path.read_text(encoding="utf-8").splitlines():
            line = line.strip()
            if len(HAN.findall(line)) >= 4:
                if clean(line):
                    texts.append(line)
                else:
                    dropped += 1
    kept, per_template = [], {}
    for t in texts:
        if len(HAN.findall(t)) < 0.6 * len(t.replace(" ", "")):
            continue
        key = NUMBER.sub("#", t)
        per_template[key] = per_template.get(key, 0) + 1
        if per_template[key] <= MAX_PER_TEMPLATE:
            kept.append(t)
    texts = kept
    random.Random(args.seed).shuffle(texts)
    with args.output.open("w", encoding="utf-8", newline="\n") as f:
        for t in texts:
            f.write(json.dumps({"text": t}, ensure_ascii=False) + "\n")
    print(f"{len(texts)} texts, {sum(map(len, texts))} characters, {dropped} held out")


if __name__ == "__main__":
    main()
