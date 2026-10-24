"""Short-input evaluation sets: 1-3 characters typed after a context, cut from the segments of
existing evaluation TSVs (id, context, text, reading[, following]). Readings are the
segment's own syllables (one per character), so they need no lexicon.

  uv run python make_short_eval.py ../tests/eval/ly_test.tsv ../tests/eval/ly_short.tsv
"""

import argparse
import random
from pathlib import Path


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--seed", type=int, default=20261024)
    args = parser.parse_args()
    rng = random.Random(args.seed)
    rows = []
    for line in args.source.read_text(encoding="utf-8").splitlines():
        parts = line.split("\t") + [""]
        rid, context, text, reading, following = parts[:5]
        syllables = reading.split()
        if len(syllables) != len(text):
            continue
        length = rng.choice((1, 1, 2, 3))  # single characters are the common short case
        if len(text) < length:
            continue
        start = rng.randrange(len(text) - length + 1)
        rows.append("\t".join([
            f"{rid}s",
            context + text[:start],
            text[start:start + length],
            " ".join(syllables[start:start + length]),
            text[start + length:] + following,
        ]))
    args.output.write_text("\n".join(rows) + "\n", encoding="utf-8", newline="\n")
    print(f"{args.output.name}: {len(rows)} rows")


if __name__ == "__main__":
    main()
