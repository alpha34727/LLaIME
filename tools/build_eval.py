"""Build the candidate-selection evaluation set from Common Voice zh-TW sentences (CC0).

Each sentence is split at non-Han characters into segments of 4-24 Han characters; a segment's
reading comes from McBopomofo's phrase table (text -> most frequent reading), segmenting the
text by dynamic programming so polyphonic characters take their in-phrase reading. Segments
that cannot be fully covered are dropped.

Output TSV columns: id, context (text before the segment in its sentence), text, reading
(syllables separated by spaces).
"""

import argparse
import random
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_LEXICON = ROOT / "data/raw/McBopomofo/Source/Data/data.txt"
DEFAULT_SOURCES = [ROOT / "data/raw/commonvoice" / f for f in
                   ("sentence-collector.txt", "setences.txt", "taipei_city_gov.txt")]

HAN = re.compile(r"[㐀-䶿一-鿿豈-﫿]+")
MAX_PHRASE = 9


def load_readings(path: Path) -> dict[str, tuple[str, float]]:
    """text -> (reading with syllables joined by '-', score), keeping the best score."""
    table: dict[str, tuple[str, float]] = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        parts = line.split(" ")
        if len(parts) != 3 or line.startswith(("#", "_")) or parts[1].startswith("MACRO@"):
            continue
        key, text, score = parts[0], parts[1], float(parts[2])
        if not HAN.fullmatch(text) or len(text) != key.count("-") + 1:
            continue
        if text not in table or table[text][1] < score:
            table[text] = (key, score)
    return table


def to_reading(text: str, table: dict[str, tuple[str, float]]) -> str | None:
    n = len(text)
    best = [float("-inf")] * (n + 1)
    back: list[tuple[int, str] | None] = [None] * (n + 1)
    best[0] = 0.0
    for i in range(n):
        if best[i] == float("-inf"):
            continue
        for j in range(i + 1, min(n, i + MAX_PHRASE) + 1):
            hit = table.get(text[i:j])
            if hit and best[i] + hit[1] > best[j]:
                best[j] = best[i] + hit[1]
                back[j] = (i, hit[0])
    if back[n] is None:
        return None
    parts = []
    pos = n
    while pos > 0:
        i, key = back[pos]
        parts.append(key.replace("-", " "))
        pos = i
    return " ".join(reversed(parts))


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("output", type=Path)
    parser.add_argument("--lexicon", type=Path, default=DEFAULT_LEXICON)
    parser.add_argument("--sources", type=Path, nargs="*", default=DEFAULT_SOURCES)
    parser.add_argument("--count", type=int, default=1000)
    parser.add_argument("--seed", type=int, default=20261014)
    args = parser.parse_args()

    table = load_readings(args.lexicon)
    rows = []
    seen = set()
    for source in args.sources:
        for sentence in source.read_text(encoding="utf-8").splitlines():
            sentence = sentence.strip()
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
    rows = rows[: args.count]
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("w", encoding="utf-8", newline="\n") as f:
        for i, (context, text, reading) in enumerate(rows):
            f.write(f"{i}\t{context}\t{text}\t{reading}\n")
    print(f"wrote {len(rows)} rows to {args.output}")


if __name__ == "__main__":
    main()
