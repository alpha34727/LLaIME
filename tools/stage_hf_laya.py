"""Assemble the Hugging Face upload folder for laya-multilingual-TW-IME (nothing is uploaded).

Files are hard-linked from the training and export outputs (same drive: no copy). The root holds
the PyTorch checkpoint for laya.load() and, with the names installer/models.json downloads,
laya.int8.onnx and tokenizer.bin. checkpoint_latest (a second copy of the weights, for resuming)
is left out. The model card is docs/huggingface/laya-multilingual-TW-IME.md with its
placeholders filled in; training/ holds the scripts and the training log.

  uv run python stage_hf_laya.py --repo-id <user>/laya-multilingual-TW-IME --project-url <url>
  hf upload-large-folder <user>/laya-multilingual-TW-IME ../build/hf/laya-multilingual-TW-IME --repo-type model
"""

import argparse
import os
import shutil
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CHECKPOINT = ROOT / "data/build/free/laya-free"
ONNX = ROOT / "data/build/free/onnx-zh"  # laya.int8.onnx and tokenizer.bin
CARD = ROOT / "docs/huggingface/laya-multilingual-TW-IME.md"
LOG = ROOT / "data/build/free/train-laya.log"
DATA = [ROOT / "docs/huggingface/DATA.md", ROOT / "data/sources.json"]
SCRIPTS = ["fetch_ly.py", "fetch_press.py", "fetch_laws.py", "gen_synthetic.py", "build_eval.py",
           "relabel_eval.py", "build_ranker_data.py", "mix_segments.py", "laya_ranker.py",
           "prune_laya_vocab.py", "export_laya_tokenizer.py", "data_manifest.py",
           "run-laya-free.cmd"]
CHECKPOINT_FILES = ["model.safetensors", "rl_agent_config.json", "questions.json",
                    "train_report.json", "encoder/config.json", "tokenizer/tokenizer.json",
                    "tokenizer/tokenizer_config.json"]
ONNX_FILES = ["laya.int8.onnx", "tokenizer.bin"]


def link(src: Path, dst: Path) -> None:
    dst.parent.mkdir(parents=True, exist_ok=True)
    dst.unlink(missing_ok=True)
    try:
        os.link(src, dst)
    except OSError:  # another drive
        shutil.copy2(src, dst)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-id", required=True, help="e.g. alice/laya-multilingual-TW-IME")
    parser.add_argument("--project-url", required=True, help="link to the LLaIME project")
    parser.add_argument("--out", type=Path, default=ROOT / "build/hf/laya-multilingual-TW-IME")
    args = parser.parse_args()

    sources = [CHECKPOINT / f for f in CHECKPOINT_FILES] + [ONNX / f for f in ONNX_FILES]
    for path in sources + [CARD, LOG] + DATA:
        if not path.exists():
            raise SystemExit(f"missing {path}")
    shutil.rmtree(args.out, ignore_errors=True)
    args.out.mkdir(parents=True)

    for f in CHECKPOINT_FILES:
        link(CHECKPOINT / f, args.out / f)
    for f in ONNX_FILES:
        link(ONNX / f, args.out / f)
    for f in DATA:
        shutil.copy2(f, args.out / f.name)

    owner = args.repo_id.split("/")[0]
    card = CARD.read_text(encoding="utf-8")
    card = (card.replace("REPO_ID", args.repo_id).replace("REPO_OWNER", owner)
            .replace("PROJECT_URL", args.project_url))
    (args.out / "README.md").write_text(card, encoding="utf-8", newline="\n")

    training = args.out / "training"
    training.mkdir()
    for name in SCRIPTS:
        shutil.copy2(ROOT / "tools" / name, training / name)
    shutil.copy2(LOG, training / "train.log")
    # The generated sentences are published (DATA.md); the open-data texts are listed there.
    shutil.copy2(ROOT / "data/raw/free/synthetic/sentences.txt", training / "synthetic_sentences.txt")
    shutil.copy2(ROOT / "data/raw/free/synthetic/provenance.json", training / "synthetic_provenance.json")

    for f in sorted(args.out.rglob("*")):
        if f.is_file():
            print(f"{f.relative_to(args.out)}  {f.stat().st_size / 2**20:,.1f} MiB")


if __name__ == "__main__":
    main()
