"""Compile McBopomofo-format data files into LLaIME's memory-mappable lexicon.bin.

Input lines: ``<key> <value> <log10 score>``. Bopomofo keys are syllables joined by ``-``;
keys starting with ``_`` (punctuation, letters, ...) go to the named table. MACRO@ values are
skipped.

Binary layout (little endian, every section 4-byte aligned), mirrored in
src/Core/Lexicon.cpp:

  Header      magic 'LLXL', version, keyCount, tonelessCount, namedCount,
              entryCount, syllablePoolCount, stringPoolCount (all u32)
  keys        keyCount x {u32 sylOffset, u16 sylLen, u16 entryCount, u32 firstEntry}
              sorted by syllable sequence (u16 lexicographic)
  toneless    tonelessCount x u32 key index, sorted by tone-stripped sequence
  named       namedCount x {u32 strOffset, u16 strLen, u16 entryCount, u32 firstEntry}
              sorted by UTF-16 code units
  syllables   u16[syllablePoolCount]
  entries     entryCount x {u32 strOffset, u16 strLen, u16 reserved, f32 score}
              per key sorted by score descending
  strings     u16[stringPoolCount] UTF-16 code units
"""

import argparse
import struct
from collections import defaultdict
from pathlib import Path

CONSONANTS = "ㄅㄆㄇㄈㄉㄊㄋㄌㄍㄎㄏㄐㄑㄒㄓㄔㄕㄖㄗㄘㄙ"
MEDIALS = "ㄧㄨㄩ"
VOWELS = "ㄚㄛㄜㄝㄞㄟㄠㄡㄢㄣㄤㄥㄦ"
TONES = {"ˊ": 2, "ˇ": 3, "ˋ": 4, "˙": 5}

TONE_SHIFT = 11
TONE_MASK = 0x7 << TONE_SHIFT


def encode_syllable(text: str) -> int:
    """Encode one Bopomofo syllable; tone 1 (unmarked) is stored as 1."""
    consonant = medial = vowel = 0
    tone = 1
    for ch in text:
        if ch in CONSONANTS:
            consonant = CONSONANTS.index(ch) + 1
        elif ch in MEDIALS:
            medial = MEDIALS.index(ch) + 1
        elif ch in VOWELS:
            vowel = VOWELS.index(ch) + 1
        elif ch in TONES:
            tone = TONES[ch]
        else:
            raise ValueError(f"not a bopomofo syllable: {text!r}")
    if not (consonant or medial or vowel):
        raise ValueError(f"empty syllable: {text!r}")
    return consonant | (medial << 5) | (vowel << 7) | (tone << TONE_SHIFT)


def utf16_units(s: str) -> list[int]:
    data = s.encode("utf-16-le")
    return list(struct.unpack(f"<{len(data) // 2}H", data))


def align4(buf: bytearray) -> None:
    while len(buf) % 4:
        buf.append(0)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("inputs", type=Path, nargs="+")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    syllable_keys: dict[tuple[int, ...], dict[str, float]] = defaultdict(dict)
    named_keys: dict[str, dict[str, float]] = defaultdict(dict)
    skipped = 0

    lines = [line for path in args.inputs for line in path.read_text(encoding="utf-8").splitlines()]
    for line in lines:
        if not line or line.startswith("#"):
            continue
        parts = line.split(" ")
        if len(parts) != 3:
            skipped += 1
            continue
        key, value, score_text = parts
        if value.startswith("MACRO@"):
            continue
        score = float(score_text)
        if key.startswith("_"):
            table = named_keys[key]
        else:
            try:
                seq = tuple(encode_syllable(s) for s in key.split("-"))
            except ValueError:
                skipped += 1
                continue
            table = syllable_keys[seq]
        # Keep the best score when a value repeats under the same key.
        if value not in table or table[value] < score:
            table[value] = score

    strings: list[int] = []
    string_offsets: dict[str, int] = {}

    def intern(s: str) -> tuple[int, int]:
        if s not in string_offsets:
            string_offsets[s] = len(strings)
            strings.extend(utf16_units(s))
        return string_offsets[s], len(utf16_units(s))

    entries = bytearray()
    entry_count = 0

    def emit_entries(values: dict[str, float]) -> tuple[int, int]:
        nonlocal entry_count
        first = entry_count
        for value, score in sorted(values.items(), key=lambda kv: (-kv[1], kv[0])):
            off, length = intern(value)
            entries.extend(struct.pack("<IHHf", off, length, 0, score))
            entry_count += 1
        return first, entry_count - first

    sorted_seqs = sorted(syllable_keys)
    syllables: list[int] = []
    keys = bytearray()
    for seq in sorted_seqs:
        first, count = emit_entries(syllable_keys[seq])
        keys.extend(struct.pack("<IHHI", len(syllables), len(seq), count, first))
        syllables.extend(seq)

    toneless_order = sorted(
        range(len(sorted_seqs)),
        key=lambda i: (tuple(s & ~TONE_MASK for s in sorted_seqs[i]), i),
    )
    toneless = struct.pack(f"<{len(toneless_order)}I", *toneless_order)

    named = bytearray()
    sorted_named = sorted(named_keys, key=utf16_units)
    for name in sorted_named:
        first, count = emit_entries(named_keys[name])
        off, length = intern(name)
        named.extend(struct.pack("<IHHI", off, length, count, first))

    out = bytearray()
    out.extend(b"LLXL")
    out.extend(struct.pack("<7I", 1, len(sorted_seqs), len(toneless_order), len(sorted_named),
                           entry_count, len(syllables), len(strings)))
    for section in (keys, toneless, named,
                    struct.pack(f"<{len(syllables)}H", *syllables),
                    entries,
                    struct.pack(f"<{len(strings)}H", *strings)):
        out.extend(section)
        align4(out)

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(out)
    print(f"keys={len(sorted_seqs)} named={len(sorted_named)} entries={entry_count} "
          f"strings={len(strings)} skipped={skipped} bytes={len(out)}")


if __name__ == "__main__":
    main()
