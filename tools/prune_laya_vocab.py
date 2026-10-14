"""Shrink a fine-tuned Laya checkpoint to the tokens Chinese input needs and export it to ONNX.

mmBERT's 256k-token embedding is 197M of Laya's 322M parameters, almost all for other
languages. Kept tokens:
  * special and byte-fallback tokens;
  * every token made only of CJK characters, Bopomofo and CJK / full-width punctuation;
  * every token that occurs when tokenizing the given corpus files (training cases, sentences,
    lexicon phrases), which covers the ASCII and other pieces Chinese text actually uses.

The tokenizer is not touched: the exported graph starts with a lookup table from original to
kept token ids (unknown ones map to <unk>), so callers keep using the original tokenizer.json.
The INT8 copy also quantizes the embedding (Gather), not only the MatMuls.

  uv run python prune_laya_vocab.py ../data/build/ranker/laya-cvwiki ../data/build/ranker/onnx-zh \
      --corpus ../data/build/ranker/train_cv_wiki.jsonl ../data/raw/zhtw-wikipedia/sentences.txt ...
"""

import argparse
import json
import os
import re
from pathlib import Path

import torch
from torch import nn

CJK = re.compile(
    r"^[⺀-⿟　-〿㄀-ㄯㆠ-ㆿ㐀-䶿一-鿿"
    r"豈-﫿︰-﹏＀-￯\U00020000-\U0003134F -⁯"
    r"·ˇˉ-ˋ˙▁]+$")
NON_ASCII = re.compile(r"[^\x00-\x7f▁]")
BYTE_TOKEN = re.compile(r"<0x[0-9A-F]{2}>")


class RemappedEmbedding(nn.Module):
    """Embedding over the kept tokens, indexed by original token ids."""

    def __init__(self, remap: torch.Tensor, weight: torch.Tensor):
        super().__init__()
        self.register_buffer("remap", remap)
        self.embedding = nn.Embedding.from_pretrained(weight, freeze=True)

    def forward(self, input_ids: torch.Tensor) -> torch.Tensor:
        return self.embedding(self.remap[input_ids])


def corpus_strings(path: Path):
    """Lines of a text file; every string value of a JSONL file."""
    def walk(value):
        if isinstance(value, str):
            yield value
        elif isinstance(value, dict):
            for k, v in value.items():
                yield from walk(k)
                yield from walk(v)
        elif isinstance(value, list):
            for v in value:
                yield from walk(v)

    with path.open(encoding="utf-8") as f:
        for line in f:
            line = line.rstrip("\n")
            if path.suffix == ".jsonl":
                yield from walk(json.loads(line))
            elif path.suffix == ".txt" and line.count(" ") == 2 and not line.startswith("#"):
                yield line.split(" ")[1]  # McBopomofo-format lexicon: the phrase
            else:
                yield line


def kept_ids(tok, corpus: list[Path]) -> list[int]:
    keep = set(tok.all_special_ids) | {tok.unk_token_id, tok.pad_token_id, tok.mask_token_id}
    for token, i in tok.get_vocab().items():
        if BYTE_TOKEN.fullmatch(token) or (CJK.match(token) and NON_ASCII.search(token)):
            keep.add(i)
    batch: list[str] = []
    for path in corpus:
        for s in corpus_strings(path):
            batch.append(s)
            if len(batch) == 4096:
                for ids in tok(batch, add_special_tokens=False)["input_ids"]:
                    keep.update(ids)
                batch.clear()
    if batch:
        for ids in tok(batch, add_special_tokens=False)["input_ids"]:
            keep.update(ids)
    return sorted(keep)


def export(model: nn.Module, path: Path) -> None:
    """Same graph and dynamic axes as export_onnx.py in the Laya repository."""
    batch, seq_len, markers = 2, 17, 3
    inputs = (
        torch.randint(0, 100, (batch, seq_len), dtype=torch.long),
        torch.ones((batch, seq_len), dtype=torch.long),
        torch.tensor([[1, 5, 9]] * batch, dtype=torch.long),
        torch.ones((batch, markers), dtype=torch.bool),
        torch.zeros(batch, dtype=torch.long),
    )
    torch.onnx.export(
        model, inputs, str(path), export_params=True, opset_version=18,
        do_constant_folding=True,
        input_names=["input_ids", "attention_mask", "marker_pos", "marker_mask", "qtype"],
        output_names=["logits", "act_logits"],
        dynamic_axes={
            "input_ids": {0: "batch_size", 1: "seq_len"},
            "attention_mask": {0: "batch_size", 1: "seq_len"},
            "marker_pos": {0: "batch_size", 1: "num_markers"},
            "marker_mask": {0: "batch_size", 1: "num_markers"},
            "qtype": {0: "batch_size"},
            "logits": {0: "batch_size", 1: "num_markers"},
            "act_logits": {0: "batch_size"},
        })


def quantize(src: Path, dst: Path) -> None:
    import onnx
    from onnxruntime.quantization import QuantType, quantize_dynamic

    # As in the Laya exporter: drop value_info that the quantizer's shape inference disagrees with.
    model = onnx.load(str(src))
    del model.graph.value_info[:]
    stripped = src.with_suffix(".noinfo.onnx")
    onnx.save(model, str(stripped))
    quantize_dynamic(str(stripped), str(dst), op_types_to_quantize=["MatMul", "Gather"],
                     weight_type=QuantType.QInt8)
    stripped.unlink()


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("checkpoint", type=Path)
    parser.add_argument("output", type=Path, help="directory for laya.onnx and laya.int8.onnx")
    parser.add_argument("--corpus", type=Path, nargs="+", required=True)
    args = parser.parse_args()

    from laya.agent import Agent

    agent = Agent(str(args.checkpoint), compile=False, device="cpu")
    tok = agent.tok
    keep = kept_ids(tok, args.corpus)
    vocab = agent.model.encoder.embeddings.tok_embeddings.weight.shape[0]
    new_index = {old: new for new, old in enumerate(keep)}
    unk = new_index[tok.unk_token_id]
    remap = torch.full((vocab,), unk, dtype=torch.long)
    remap[torch.tensor(keep)] = torch.arange(len(keep))
    weight = agent.model.encoder.embeddings.tok_embeddings.weight.detach()[torch.tensor(keep)]
    agent.model.encoder.embeddings.tok_embeddings = RemappedEmbedding(remap, weight.clone())
    agent.model.eval()
    print(f"kept {len(keep)} of {vocab} tokens")

    args.output.mkdir(parents=True, exist_ok=True)
    fp32 = args.output / "laya.onnx"
    export(agent.model, fp32)
    quantize(fp32, args.output / "laya.int8.onnx")
    (args.output / "kept_tokens.json").write_text(json.dumps(keep), encoding="utf-8")
    for f in sorted(os.listdir(args.output)):
        print(f, os.path.getsize(args.output / f) // 1_000_000, "MB")


if __name__ == "__main__":
    main()
