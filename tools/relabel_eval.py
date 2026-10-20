"""Re-derive the readings of evaluation TSVs (id, context, text, reading) from other lexicons.

Used to take the Ministry of Education dictionary out of the evaluation sets' readings: the
rows keep their ids, contexts and texts; rows the given lexicons cannot cover are dropped and
counted. Readings come from build_eval.to_reading (most frequent in-phrase readings).

  uv run python relabel_eval.py ../tests/eval/commonvoice_test.tsv \
      --lexicon ../data/build/readings-free.txt
"""

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from build_eval import load_readings, to_reading  # noqa: E402


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("tsv", type=Path, nargs="+", help="rewritten in place")
    parser.add_argument("--lexicon", type=Path, nargs="+", required=True)
    args = parser.parse_args()
    table: dict[str, tuple[str, float]] = {}
    for path in args.lexicon:
        for text, (key, score) in load_readings(path).items():
            if text not in table or table[text][1] < score:
                table[text] = (key, score)
    for path in args.tsv:
        rows, dropped, changed = [], 0, 0
        for line in path.read_text(encoding="utf-8").splitlines():
            parts = line.split("\t")
            reading = to_reading(parts[2], table)
            if reading is None:
                dropped += 1
                continue
            changed += reading != parts[3]
            rows.append("\t".join(parts[:3] + [reading]))
        path.write_text("\n".join(rows) + "\n", encoding="utf-8", newline="\n")
        print(f"{path.name}: {len(rows)} rows, {changed} readings changed, {dropped} dropped")


if __name__ == "__main__":
    main()
