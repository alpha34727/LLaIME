"""Collect phrases missing from McBopomofo's data.txt and write them in the same format.

Source: libchewing-data tsi.csv (LGPL-2.1-or-later), phrases with corpus frequencies.

Only phrases of two or more Han characters whose text is not already in McBopomofo are
added, so McBopomofo's readings and scores stay authoritative. Chewing frequencies are mapped
onto McBopomofo's log10 scale by a linear fit over the phrases both sources share, and never
score below McBopomofo's own score for unattested phrases of the same length.

(The MOE Revised Mandarin Dictionary was tried as a second source and dropped: its ~84k extra
headwords are mostly literary and rare.)
"""

import argparse
import csv
import math
import re
import statistics
from pathlib import Path

HAN = re.compile(r"[㐀-䶿一-鿿豈-﫿\U00020000-\U0003134F]+")
BOPOMOFO = "ㄅㄆㄇㄈㄉㄊㄋㄌㄍㄎㄏㄐㄑㄒㄓㄔㄕㄖㄗㄘㄙㄧㄨㄩㄚㄛㄜㄝㄞㄟㄠㄡㄢㄣㄤㄥㄦ"
TONES = "ˊˇˋ˙"
# Chewing phrases seen fewer times than this are mostly segmentation fragments (賺有, 熔於).
MIN_CHEWING_FREQ = 1


def unattested_score(length: int) -> float:
    """McBopomofo's score for a phrase without frequency data (-7.47 for two characters,
    rising 0.431 per character)."""
    return round(-8.3353 + 0.43136 * length, 8)


def normalize_syllable(s: str) -> str | None:
    """Tone mark last, as McBopomofo writes it, wherever the source put it."""
    tone = "".join(c for c in s if c in TONES)
    body = "".join(c for c in s if c not in TONES)
    if not body or len(tone) > 1 or any(c not in BOPOMOFO for c in body):
        return None
    return body + tone


def load_mcbopomofo(path: Path) -> dict[str, float]:
    texts: dict[str, float] = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        parts = line.split(" ")
        if len(parts) != 3 or line.startswith(("#", "_")):
            continue
        try:
            score = float(parts[2])
        except ValueError:
            continue
        texts[parts[1]] = max(score, texts.get(parts[1], -99.0))
    return texts


def load_chewing(path: Path) -> list[tuple[str, int, str]]:
    out = []
    with path.open(encoding="utf-8") as f:
        for row in csv.reader(line for line in f if not line.startswith("#")):
            if len(row) < 3:
                continue
            text, freq, reading = row[0], row[1], row[2]
            if len(text) < 2 or not HAN.fullmatch(text) or not freq.isdigit():
                continue
            syllables = [normalize_syllable(s) for s in reading.split()]
            if len(syllables) != len(text) or any(s is None for s in syllables):
                continue
            out.append((text, int(freq), "-".join(syllables)))
    return out


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--mcbopomofo", type=Path, required=True, help="McBopomofo data.txt")
    parser.add_argument("--chewing", type=Path, required=True, help="libchewing-data tsi.csv")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    known = load_mcbopomofo(args.mcbopomofo)
    added: dict[tuple[str, str], float] = {}

    chewing = load_chewing(args.chewing)
    shared = [(math.log10(freq + 1), known[text]) for text, freq, _ in chewing
              if text in known and len(text) >= 2]
    fit = statistics.linear_regression([x for x, _ in shared], [y for _, y in shared])
    for text, freq, key in chewing:
        if text in known or freq < MIN_CHEWING_FREQ:
            continue
        score = max(unattested_score(len(text)),
                    round(fit.intercept + fit.slope * math.log10(freq + 1), 8))
        added[(key, text)] = score

    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("w", encoding="utf-8", newline="\n") as f:
        for (key, text), score in sorted(added.items()):
            f.write(f"{key} {text} {score}\n")
    texts = {text for _, text in added}
    print(f"supplement: {len(added)} entries, {len(texts)} phrases "
          f"(chewing fit slope {fit.slope:.3f} "
          f"intercept {fit.intercept:.3f})")


if __name__ == "__main__":
    main()
