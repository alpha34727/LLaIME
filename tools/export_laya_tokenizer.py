"""Write a Laya checkpoint's tokenizer and input settings for the C++ engine (LayaScorer).

  tokenizer.bin   compact form of tokenizer.json (mmBERT: Metaspace + BPE with byte fallback),
                  mirrored in src/Engine/LayaTokenizer.cpp:
                    magic 'LLTK', version, vocabSize, charCount, mergeCount, unk, cls, sep, mask,
                    pad, maxLen, headMaxLen (u32 each)
                    byteTokens  u32[256]         id of <0xXX>, or 0xFFFFFFFF if absent
                    chars       {u32 codepoint, u32 id}[charCount], sorted by codepoint
                    merges      {u32 left, u32 right, u32 result}[mergeCount], in rank order
                    instruction u32 length + UTF-8 bytes (the question's instructions)
  expected.tsv    with --cases: the input ids and marker positions Laya itself builds for each
                  case (one line per case: id, ids, markers), to check the C++ port against.
"""

import argparse
import json
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from build_ranker_data import INSTRUCTIONS  # noqa: E402


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("checkpoint", type=Path)
    parser.add_argument("output", type=Path, help="directory")
    parser.add_argument("--cases", type=Path, nargs="*", default=[])
    args = parser.parse_args()

    spec = json.loads((args.checkpoint / "tokenizer" / "tokenizer.json").read_text(encoding="utf-8"))
    cfg = json.loads((args.checkpoint / "rl_agent_config.json").read_text(encoding="utf-8"))
    model = spec["model"]
    assert model["type"] == "BPE" and model["byte_fallback"] and not model["ignore_merges"]
    assert spec["pre_tokenizer"] == {"type": "Metaspace", "replacement": "▁",
                                     "prepend_scheme": "always", "split": True}
    vocab: dict[str, int] = model["vocab"]
    for added in spec["added_tokens"]:
        vocab.setdefault(added["content"], added["id"])
    special = {name: vocab[tok] for name, tok in
               (("unk", "<unk>"), ("cls", "<bos>"), ("sep", "<eos>"), ("mask", "<mask>"),
                ("pad", "<pad>"))}
    # <0x09> is missing (tab is an ordinary token); a character needing a missing byte token
    # becomes <unk>, as in Hugging Face tokenizers.
    byte_tokens = [vocab.get(f"<0x{b:02X}>", 0xFFFFFFFF) for b in range(256)]
    chars = sorted((ord(t), i) for t, i in vocab.items() if len(t) == 1)
    merges = []
    for merge in model["merges"]:
        left, right = merge if isinstance(merge, list) else merge.split(" ", 1)
        merges.append((vocab[left], vocab[right], vocab[left + right]))
    instruction = INSTRUCTIONS.encode("utf-8")

    out = bytearray(b"LLTK")
    out += struct.pack("<11I", 1, max(vocab.values()) + 1, len(chars), len(merges),
                       special["unk"], special["cls"], special["sep"], special["mask"],
                       special["pad"], cfg["max_len"], cfg["head_max_len"])
    out += struct.pack("<256I", *byte_tokens)
    for c in chars:
        out += struct.pack("<II", *c)
    for m in merges:
        out += struct.pack("<III", *m)
    out += struct.pack("<I", len(instruction)) + instruction
    args.output.mkdir(parents=True, exist_ok=True)
    (args.output / "tokenizer.bin").write_bytes(out)
    print(f"tokenizer.bin: {len(chars)} chars, {len(merges)} merges, {len(out) // 1000} kB")

    if args.cases:
        from transformers import AutoTokenizer
        from laya.common import build_sequence

        tok = AutoTokenizer.from_pretrained(str(args.checkpoint / "tokenizer"))
        with (args.output / "expected.tsv").open("w", encoding="utf-8", newline="\n") as f:
            for path in args.cases:
                for line in path.read_text(encoding="utf-8").splitlines():
                    case = json.loads(line)
                    q = next(iter(case["questions"].values()))
                    internal = {"t": q["type"], "ins": q["instructions"], "crit": q["criteria"]}
                    ids, markers = build_sequence(tok, case["state"], internal,
                                                  max_len=cfg["max_len"],
                                                  head_max_len=cfg["head_max_len"])
                    f.write("\t".join([case["id"], json.dumps(case["state"], ensure_ascii=False),
                                       json.dumps(list(q["criteria"]), ensure_ascii=False),
                                       " ".join(map(str, ids)), " ".join(map(str, markers))])
                            + "\n")


if __name__ == "__main__":
    main()
