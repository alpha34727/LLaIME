"""Add the text after each segment (column 5) to evaluation TSVs (id, context, text, reading).

A row's segment was cut from a source sentence at position len(context); the following text is
the rest of that sentence, so the IME's look at the text after the cursor can be evaluated.
Rows whose sentence cannot be found keep an empty fifth column (and are counted).

  uv run python add_following_context.py ../tests/eval/ly_test.tsv \
      --sources ../data/raw/free/ly/sentences.txt
"""

import argparse
from collections import defaultdict
from pathlib import Path

KEY = 4  # segments have at least 4 characters


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("tsv", type=Path, help="rewritten in place")
    parser.add_argument("--sources", type=Path, nargs="+", required=True)
    args = parser.parse_args()

    index: dict[str, list[str]] = defaultdict(list)
    for path in args.sources:
        for sentence in path.read_text(encoding="utf-8").splitlines():
            sentence = sentence.strip()
            if len(sentence) >= KEY:
                index[sentence[:KEY]].append(sentence)

    rows, missing, ambiguous, nonempty = [], 0, 0, 0
    for line in args.tsv.read_text(encoding="utf-8").splitlines():
        parts = line.split("\t")[:4]
        head = parts[1] + parts[2]
        after = {s[len(head):] for s in index.get(head[:KEY], ()) if s.startswith(head)}
        if not after:
            missing += 1
        ambiguous += len(after) > 1
        # Several source sentences can share the head; the shortest rest is the conservative one.
        following = min(after, key=len) if after else ""
        nonempty += bool(following)
        rows.append("\t".join(parts + [following]))
    args.tsv.write_text("\n".join(rows) + "\n", encoding="utf-8", newline="\n")
    print(f"{args.tsv.name}: {len(rows)} rows, {nonempty} with following text, "
          f"{missing} sentences not found, {ambiguous} ambiguous")


if __name__ == "__main__":
    main()
