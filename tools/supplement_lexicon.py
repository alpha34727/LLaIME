"""Collect phrases missing from McBopomofo's data.txt and write them in the same format.

Sources:
  * Ministry of Education, Revised Mandarin Chinese Dictionary (重編國語辭典修訂本), CC BY-ND
    3.0 TW: headwords with their standard Taiwan readings (main, 又音, 語音, 讀音, 或讀).
  * libchewing-data tsi.csv, LGPL-2.1-or-later: phrases with corpus frequencies.

Only phrases of two or more Han characters whose text is not already in McBopomofo are
added, so McBopomofo's readings and scores stay authoritative. The dictionary has no
frequencies; its phrases get McBopomofo's own score for unattested phrases of the same
length. Chewing frequencies are mapped onto McBopomofo's log10 scale by a linear fit over the
phrases both sources share.
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
# Dictionary variant kinds (變體類型) kept as extra readings. 變 (sandhi and erhua-reduced
# forms) is left out: its erhua spellings do not map onto per-character readings.
VARIANT_KINDS = {"又音", "語音", "讀音", "或讀"}
# Chewing phrases seen fewer times than this are mostly segmentation fragments (賺有, 熔於).
MIN_CHEWING_FREQ = 1


def unattested_score(length: int) -> float:
    """McBopomofo's score for a phrase without frequency data (-7.47 for two characters,
    rising 0.431 per character)."""
    return round(-8.3353 + 0.43136 * length, 8)


def normalize_syllable(s: str) -> str | None:
    """Tone mark last, as McBopomofo writes it. MOE puts the neutral tone first (˙ㄇㄜ) and
    an erhua syllable's tone before its ㄦ (ㄅㄟˋㄦ)."""
    tone = "".join(c for c in s if c in TONES)
    body = "".join(c for c in s if c not in TONES)
    if not body or len(tone) > 1 or any(c not in BOPOMOFO for c in body):
        return None
    return body + tone


def split_reading(text: str, reading: str) -> list[str] | None:
    """Per-character syllables, or None. Erhua is written on the preceding syllable
    (巴兒狗: ㄅㄚㄦ ㄍㄡˇ); 兒 then gets its own syllable ㄦˊ, as it is typed."""
    syllables = [normalize_syllable(s) for s in reading.split()]
    if not syllables or any(s is None for s in syllables):
        return None
    out: list[str] = []
    i = 0
    for pos, ch in enumerate(text):
        if i >= len(syllables):
            return None
        if ch == "兒" and pos > 0 and out[-1].rstrip(TONES).endswith("ㄦ") and \
                len(out[-1].rstrip(TONES)) > 1:
            prev = out[-1]
            body = prev.rstrip(TONES)
            out[-1] = body[:-1] + prev[len(body):]
            out.append("ㄦˊ")
            continue
        out.append(syllables[i])
        i += 1
    if i != len(syllables):
        # A merged erhua syllable at the end of the text (…尖兒) without 兒 following.
        return None
    return out


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


def load_moe(path: Path) -> dict[str, set[str]]:
    import openpyxl

    readings: dict[str, set[str]] = {}
    workbook = openpyxl.load_workbook(path, read_only=True)
    rows = workbook.active.iter_rows(values_only=True)
    header = [str(h) for h in next(rows)]
    col_text = header.index("字詞名")
    col_main = header.index("注音一式")
    col_kind = next(i for i, h in enumerate(header) if h.startswith("變體類型"))
    col_variant = header.index("變體注音")
    for row in rows:
        text = (row[col_text] or "").strip()
        if len(text) < 2 or not HAN.fullmatch(text):
            continue
        candidates = [row[col_main] or ""]
        if (row[col_kind] or "").strip() in VARIANT_KINDS:
            candidates.append(row[col_variant] or "")
        for reading in candidates:
            syllables = split_reading(text, reading)
            if syllables:
                readings.setdefault(text, set()).add("-".join(syllables))
    return readings


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
    parser.add_argument("--moe", type=Path, required=True, help="dict_revised_*.xlsx")
    parser.add_argument("--chewing", type=Path, required=True, help="libchewing-data tsi.csv")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    known = load_mcbopomofo(args.mcbopomofo)
    added: dict[tuple[str, str], float] = {}

    moe = load_moe(args.moe)
    for text, keys in moe.items():
        if text in known:
            continue
        for key in keys:
            added[(key, text)] = unattested_score(len(text))
    moe_count = len(added)

    chewing = load_chewing(args.chewing)
    shared = [(math.log10(freq + 1), known[text]) for text, freq, _ in chewing
              if text in known and len(text) >= 2]
    fit = statistics.linear_regression([x for x, _ in shared], [y for _, y in shared])
    for text, freq, key in chewing:
        if text in known or freq < MIN_CHEWING_FREQ:
            continue
        score = max(unattested_score(len(text)),
                    round(fit.intercept + fit.slope * math.log10(freq + 1), 8))
        added[(key, text)] = max(score, added.get((key, text), -99.0))

    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("w", encoding="utf-8", newline="\n") as f:
        for (key, text), score in sorted(added.items()):
            f.write(f"{key} {text} {score}\n")
    texts = {text for _, text in added}
    print(f"supplement: {len(added)} entries, {len(texts)} phrases "
          f"(dictionary {moe_count}, chewing fit slope {fit.slope:.3f} "
          f"intercept {fit.intercept:.3f})")


if __name__ == "__main__":
    main()
