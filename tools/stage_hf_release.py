"""Assemble the Hugging Face upload folder for Qwen3.5-2B-TW-IME (nothing is uploaded).

The weights are hard-linked from where the training run left them (same drive: no copy; the
GGUF is given its published name), the
model card is docs/huggingface/Qwen3.5-2B-TW-IME.md with its two placeholders filled in, and the
training scripts and a trimmed training log go under training/. lora_qwen.py copies the base
model's README and LICENSE into its output folder; they are replaced here by the model card and
the same Apache-2.0 text.

  uv run python stage_hf_release.py --repo-id <user>/Qwen3.5-2B-TW-IME --project-url <url>
  hf upload-large-folder <user>/Qwen3.5-2B-TW-IME ../build/hf/Qwen3.5-2B-TW-IME --repo-type model
"""

import argparse
import os
import re
import shutil
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
FREE = ROOT / "data/build/free"
# The data scripts (sources, licenses and hashes in DATA.md / sources.json) and the training.
SCRIPTS = ["fetch_ly.py", "fetch_press.py", "fetch_laws.py", "gen_synthetic.py", "build_eval.py",
           "build_lm_corpus.py", "lora_qwen.py", "data_manifest.py"]
# Taken from the base model by lora_qwen.py, or not part of the release.
SKIP = {"README.md", ".gitattributes", "LICENSE"}
LOG_LINE = re.compile(r"^(train |LoRA targets|trainable params|eval loss|step |saved )")


def link(src: Path, dst: Path) -> None:
    dst.parent.mkdir(parents=True, exist_ok=True)
    dst.unlink(missing_ok=True)
    try:
        os.link(src, dst)
    except OSError:  # another drive
        shutil.copy2(src, dst)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--size", choices=["2B", "0.8B"], default="2B")
    parser.add_argument("--repo-id", required=True, help="e.g. alice/Qwen3.5-2B-TW-IME")
    parser.add_argument("--project-url", required=True, help="link to the LLaIME project")
    parser.add_argument("--out", type=Path, help="default: build/hf/Qwen3.5-<size>-TW-IME")
    args = parser.parse_args()
    name = f"Qwen3.5-{args.size}-TW-IME"
    args.out = args.out or ROOT / "build/hf" / name
    merged = FREE / f"qwen3.5-{args.size.lower()}-tw-free"
    gguf = ROOT / "models/tw-free" / f"{name}-Q4_K_M.gguf"  # the name installer/models.json downloads
    card_path = ROOT / "docs/huggingface" / f"{name}.md"
    log = FREE / ("run-lora.log" if args.size == "2B" else "run-lora-0.8b.log")
    data_md, sources = ROOT / "docs/huggingface/DATA.md", ROOT / "data/sources.json"

    for path in (merged / "model.safetensors", gguf, card_path, log, data_md, sources):
        if not path.exists():
            raise SystemExit(f"missing {path}")
    shutil.rmtree(args.out, ignore_errors=True)
    args.out.mkdir(parents=True)

    for f in merged.iterdir():
        if f.is_file() and f.name not in SKIP:
            link(f, args.out / f.name)
    link(gguf, args.out / gguf.name)
    shutil.copy2(ROOT / f"models/hf/Qwen3.5-{args.size}/LICENSE", args.out / "LICENSE")
    shutil.copy2(data_md, args.out / "DATA.md")
    shutil.copy2(sources, args.out / "sources.json")

    card = card_path.read_text(encoding="utf-8")
    card = (card.replace("REPO_ID", args.repo_id).replace("REPO_OWNER", args.repo_id.split("/")[0])
            .replace("PROJECT_URL", args.project_url))
    (args.out / "README.md").write_text(card, encoding="utf-8", newline="\n")

    training = args.out / "training"
    training.mkdir()
    for name in SCRIPTS:
        shutil.copy2(ROOT / "tools" / name, training / name)
    # The generated sentences are published (DATA.md); the open-data texts are listed there.
    shutil.copy2(ROOT / "data/raw/free/synthetic/sentences.txt", training / "synthetic_sentences.txt")
    shutil.copy2(ROOT / "data/raw/free/synthetic/provenance.json", training / "synthetic_provenance.json")
    # Without the timestamp line (a mangled local-time string) and the evaluation output.
    lines = [l for l in log.read_text(encoding="utf-8", errors="replace").splitlines()
             if LOG_LINE.match(l)]
    (training / "train.log").write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")

    for f in sorted(args.out.rglob("*")):
        if f.is_file():
            print(f"{f.relative_to(args.out)}  {f.stat().st_size / 2**20:,.1f} MiB")


if __name__ == "__main__":
    main()
