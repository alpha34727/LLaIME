"""Turn text into training cases for a candidate-sentence ranker (stage 4b, Laya).

Step 1 (`segments`): split source sentences into 4-24 character Han segments with readings,
like build_eval.py, leaving out every sentence that contains a segment of an evaluation set.
The output TSV feeds `LLaIME.Eval --dump`, which writes the lexicon's N best conversions.

Step 2 (`cases`): join that dump with the segments and write Laya JSONL cases: the state is
the text before the segment (see state()), and one `choice` question whose options are the candidate
sentences. Rows whose N best list misses the true sentence, or has a single candidate, are
skipped: there is nothing to learn from them.
"""

import argparse
import json
import random
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from build_eval import HAN, load_readings, to_reading  # noqa: E402

QUESTION = "sentence"
INSTRUCTIONS = "哪一個是這段注音最自然的台灣繁體中文寫法？"


def state(context: str) -> str:
    """Laya skips cases with an empty state, and most segments start a sentence."""
    return "上文：" + (context or "無")


def segments(args: argparse.Namespace) -> None:
    held_out = set()
    for path in args.exclude:
        for line in path.read_text(encoding="utf-8").splitlines():
            parts = line.split("\t")
            if len(parts) >= 3:
                held_out.add(parts[2])
    table: dict[str, tuple[str, float]] = {}
    for path in args.lexicon:
        for text, (key, score) in load_readings(path).items():
            if text not in table or table[text][1] < score:
                table[text] = (key, score)

    rows, seen, skipped = [], set(), 0
    for source in args.sources:
        for sentence in source.read_text(encoding="utf-8").splitlines():
            sentence = sentence.strip()
            if any(h in sentence for h in held_out if len(h) <= len(sentence)):
                skipped += 1
                continue
            for m in HAN.finditer(sentence):
                text = m.group()
                if not 4 <= len(text) <= 24 or text in seen:
                    continue
                reading = to_reading(text, table)
                if reading is None:
                    continue
                seen.add(text)
                rows.append((sentence[: m.start()], text, reading))
    random.Random(args.seed).shuffle(rows)
    with args.output.open("w", encoding="utf-8", newline="\n") as f:
        for i, (context, text, reading) in enumerate(rows):
            f.write(f"r{i}\t{context}\t{text}\t{reading}\n")
    print(f"{len(rows)} segments, {skipped} sentences held out")


def cases(args: argparse.Namespace) -> None:
    contexts = {}
    for line in args.segments.read_text(encoding="utf-8").splitlines():
        parts = line.split("\t")
        contexts[parts[0]] = parts[1]
    written = skipped = 0
    with args.output.open("w", encoding="utf-8", newline="\n") as f:
        for line in args.dump.read_text(encoding="utf-8").splitlines():
            parts = line.split("\t")
            truth = int(parts[3])
            options = [c.split("|")[0] for c in parts[4:]]
            if truth < 0 or len(options) < 2:
                skipped += 1
                continue
            case = {
                "id": parts[0],
                "state": state(contexts.get(parts[0], "")),
                "questions": {QUESTION: {"type": "choice", "instructions": INSTRUCTIONS,
                                         "criteria": {o: "" for o in options}}},
                "expected": {QUESTION: options[truth]},
            }
            f.write(json.dumps(case, ensure_ascii=False) + "\n")
            written += 1
    print(f"{written} cases, {skipped} skipped")


def main() -> None:
    parser = argparse.ArgumentParser()
    sub = parser.add_subparsers(dest="step", required=True)
    s = sub.add_parser("segments")
    s.add_argument("output", type=Path)
    s.add_argument("--sources", type=Path, nargs="+", required=True)
    s.add_argument("--lexicon", type=Path, nargs="+", required=True,
                   help="McBopomofo-format files for readings")
    s.add_argument("--exclude", type=Path, nargs="*", default=[], help="evaluation TSVs")
    s.add_argument("--seed", type=int, default=20261015)
    c = sub.add_parser("cases")
    c.add_argument("output", type=Path)
    c.add_argument("--segments", type=Path, required=True)
    c.add_argument("--dump", type=Path, required=True, help="LLaIME.Eval --dump output")
    args = parser.parse_args()
    segments(args) if args.step == "segments" else cases(args)


if __name__ == "__main__":
    main()
