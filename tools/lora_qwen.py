"""LoRA continued pre-training of the ranking LM on Taiwan Traditional Chinese (stage 4b).

Each example is the engine's scoring prompt followed by a text from build_lm_corpus.py:
"以下是一段台灣繁體中文的文字：\\n" + text, with the loss on the text only, which is exactly
the quantity the engine computes, log P(candidate | prompt + context). LoRA adapters go on every
linear layer of the language model (attention, linear attention and MLP); the vision tower is
left alone. The merged model is saved in the original Hugging Face layout, ready for llama.cpp's
convert_hf_to_gguf.py.

  uv run python lora_qwen.py ../models/hf/Qwen3.5-2B ../data/build/lm/corpus.jsonl \\
      ../data/build/lm/qwen3.5-2b-tw --max-texts 40000
"""

import argparse
import json
import math
import random
import shutil
import time
from pathlib import Path

import torch
from torch import nn

PROMPT = "以下是一段台灣繁體中文的文字：\n"  # kPromptPrefix in src/Engine/EngineMain.cpp


def linear_targets(model: nn.Module) -> list[str]:
    """Leaf names of the language model's linear layers (not the vision tower or LM head)."""
    names = set()
    for name, module in model.named_modules():
        if isinstance(module, nn.Linear) and "language_model" in name and "lm_head" not in name:
            names.add(name.rsplit(".", 1)[-1])
    return sorted(names)


def batches(examples: list[list[int]], prompt_len: int, batch_size: int, pad: int):
    for i in range(0, len(examples), batch_size):
        chunk = examples[i:i + batch_size]
        width = max(map(len, chunk))
        ids = torch.full((len(chunk), width), pad, dtype=torch.long)
        labels = torch.full((len(chunk), width), -100, dtype=torch.long)
        mask = torch.zeros((len(chunk), width), dtype=torch.long)
        for row, ex in enumerate(chunk):
            ids[row, :len(ex)] = torch.tensor(ex)
            labels[row, prompt_len:len(ex)] = torch.tensor(ex[prompt_len:])
            mask[row, :len(ex)] = 1
        yield ids, labels, mask


@torch.no_grad()
def evaluate(model, examples, prompt_len, pad, batch_size) -> float:
    model.eval()
    total = count = 0.0
    for ids, labels, mask in batches(examples, prompt_len, batch_size, pad):
        ids, labels, mask = ids.cuda(), labels.cuda(), mask.cuda()
        with torch.autocast("cuda", dtype=torch.bfloat16):
            out = model(input_ids=ids, attention_mask=mask, labels=labels)
        n = (labels[:, 1:] != -100).sum().item()
        total += out.loss.item() * n
        count += n
    model.train()
    return total / count


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("base", type=Path)
    parser.add_argument("corpus", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--max-texts", type=int, default=0, help="0: all")
    parser.add_argument("--eval-texts", type=int, default=500)
    parser.add_argument("--batch-size", type=int, default=16)
    parser.add_argument("--micro-batch", type=int, default=8,
                        help="per forward pass: the logits over the 248k-token vocabulary are large")
    parser.add_argument("--lr", type=float, default=1e-4)
    parser.add_argument("--rank", type=int, default=16)
    parser.add_argument("--max-tokens", type=int, default=256)
    parser.add_argument("--seed", type=int, default=20261015)
    args = parser.parse_args()

    from peft import LoraConfig, get_peft_model
    from transformers import AutoModelForImageTextToText, AutoTokenizer

    torch.manual_seed(args.seed)
    tok = AutoTokenizer.from_pretrained(args.base)
    texts = [json.loads(line)["text"] for line in args.corpus.read_text(encoding="utf-8").splitlines()]
    eval_texts, train_texts = texts[:args.eval_texts], texts[args.eval_texts:]
    if args.max_texts:
        train_texts = train_texts[:args.max_texts]
    prompt_ids = tok(PROMPT, add_special_tokens=False)["input_ids"]

    def encode(t: str) -> list[int]:
        return (prompt_ids + tok(t, add_special_tokens=False)["input_ids"])[:args.max_tokens]

    train = [encode(t) for t in train_texts]
    evals = [encode(t) for t in eval_texts]
    # Similar lengths per batch, batches in random order.
    train.sort(key=len)
    groups = [train[i:i + args.batch_size] for i in range(0, len(train), args.batch_size)]
    random.Random(args.seed).shuffle(groups)
    train = [ex for g in groups for ex in g]
    pad = tok.pad_token_id if tok.pad_token_id is not None else tok.eos_token_id
    tokens = sum(len(ex) - len(prompt_ids) for ex in train)
    print(f"train {len(train)} texts ({tokens} tokens), eval {len(evals)}")

    model = AutoModelForImageTextToText.from_pretrained(args.base, dtype=torch.bfloat16).cuda()
    model.gradient_checkpointing_enable()
    model.enable_input_require_grads()
    targets = linear_targets(model)
    print("LoRA targets:", targets)
    lora = LoraConfig(r=args.rank, lora_alpha=2 * args.rank, lora_dropout=0.05,
                      target_modules=rf".*language_model.*\.({'|'.join(targets)})")
    model = get_peft_model(model, lora)
    model.print_trainable_parameters()

    before = evaluate(model, evals, len(prompt_ids), pad, args.micro_batch)
    print(f"eval loss before {before:.4f}")
    params = [p for p in model.parameters() if p.requires_grad]
    optimizer = torch.optim.AdamW(params, lr=args.lr, weight_decay=0.0)
    steps = math.ceil(len(train) / args.batch_size)
    warmup = max(1, steps // 20)
    schedule = torch.optim.lr_scheduler.LambdaLR(
        optimizer, lambda s: min(1.0, (s + 1) / warmup) * 0.5 * (1 + math.cos(math.pi * s / steps)))
    model.train()
    start = time.time()
    for step in range(steps):
        batch = train[step * args.batch_size:(step + 1) * args.batch_size]
        targets_total = sum(len(ex) - len(prompt_ids) for ex in batch)
        for ids, labels, mask in batches(batch, len(prompt_ids), args.micro_batch, pad):
            ids, labels, mask = ids.cuda(), labels.cuda(), mask.cuda()
            with torch.autocast("cuda", dtype=torch.bfloat16):
                loss = model(input_ids=ids, attention_mask=mask, labels=labels).loss
            # Weight each micro-batch by its share of the batch's predicted tokens.
            share = (labels[:, 1:] != -100).sum().item() / max(1, targets_total)
            (loss * share).backward()
        torch.nn.utils.clip_grad_norm_(params, 1.0)
        optimizer.step()
        schedule.step()
        optimizer.zero_grad(set_to_none=True)
        if step % 50 == 0 or step == steps - 1:
            elapsed = time.time() - start
            print(f"step {step + 1}/{steps} loss {loss.item():.4f} "
                  f"{elapsed / (step + 1):.2f} s/step, eta {(steps - step - 1) * elapsed / (step + 1) / 60:.0f} min",
                  flush=True)
    after = evaluate(model, evals, len(prompt_ids), pad, args.micro_batch)
    print(f"eval loss before {before:.4f} after {after:.4f}")

    merged = model.merge_and_unload()
    args.output.mkdir(parents=True, exist_ok=True)
    merged.save_pretrained(args.output, safe_serialization=True)
    for f in args.base.iterdir():  # tokenizer, processor and template files
        if f.is_file() and not f.name.startswith("model") and not (args.output / f.name).exists():
            shutil.copy2(f, args.output / f.name)
    print(f"saved {args.output}")


if __name__ == "__main__":
    main()
