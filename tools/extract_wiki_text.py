"""Extract plain sentences from zetavg/zh-tw-wikipedia parquet shards (CC BY-SA 3.0/4.0).

The shards hold Wikipedia's zh-tw rendering as Markdown. Headings, list items, tables and
links are dropped; paragraphs are split into sentences at 。！？；. Output: one sentence per
line, for build_ranker_data.py.
"""

import argparse
import random
import re
from pathlib import Path

import pyarrow.parquet as pq

SENTENCE_END = re.compile(r"(?<=[。！？；])")
MARKUP = re.compile(r"\*\*|__|`|\[\^?\d+\]")
LINK = re.compile(r"\[([^\]]*)\]\([^)]*\)")


def sentences(markdown: str):
    for line in markdown.splitlines():
        line = line.strip()
        if not line or line[0] in "#*-|>!<" or line[0].isdigit():
            continue
        line = MARKUP.sub("", LINK.sub(r"\1", line))
        for s in SENTENCE_END.split(line):
            s = s.strip()
            if 6 <= len(s) <= 120:
                yield s


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("shards", type=Path, nargs="+")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--count", type=int, default=100000)
    parser.add_argument("--seed", type=int, default=20261015)
    args = parser.parse_args()
    pool = []
    for shard in args.shards:
        for text in pq.read_table(shard, columns=["markdown"]).column("markdown").to_pylist():
            pool.extend(sentences(text or ""))
    random.Random(args.seed).shuffle(pool)
    args.output.write_text("\n".join(pool[: args.count]) + "\n", encoding="utf-8")
    print(f"{min(len(pool), args.count)} of {len(pool)} sentences")


if __name__ == "__main__":
    main()
