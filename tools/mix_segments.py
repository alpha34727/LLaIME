"""Mix per-source segment TSVs (build_ranker_data.py segments) into one, with a quota per source.

Each argument is <tsv>:<count>; count 0 takes the whole file. Rows are sampled with a fixed
seed, renumbered with the source name as prefix (ly12, cv3, ...) so cases can be traced back,
and shuffled together.

  uv run python mix_segments.py ../data/build/free/segments.tsv \
      ../data/build/free/seg_cv.tsv:0 ../data/build/free/seg_ly.tsv:40000 ...
"""

import argparse
import random
from pathlib import Path


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("output", type=Path)
    parser.add_argument("sources", nargs="+", help="<tsv>:<count>")
    parser.add_argument("--seed", type=int, default=20261020)
    args = parser.parse_args()
    rng = random.Random(args.seed)
    rows = []
    for spec in args.sources:
        path, count = spec.rsplit(":", 1)
        path = Path(path)
        name = path.stem.removeprefix("seg_")
        lines = path.read_text(encoding="utf-8").splitlines()
        if int(count) and int(count) < len(lines):
            lines = rng.sample(lines, int(count))
        for i, line in enumerate(lines):
            rows.append(f"{name}{i}\t" + line.split("\t", 1)[1])
        print(f"{name}: {len(lines)}")
    rng.shuffle(rows)
    args.output.write_text("\n".join(rows) + "\n", encoding="utf-8", newline="\n")
    print(f"{len(rows)} segments")


if __name__ == "__main__":
    main()
