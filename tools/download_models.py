"""Download the GGUF models used by the candidate-ranking evaluation into models/.

All are Apache-2.0. Pass model names to download a subset; with no arguments everything listed
is fetched. Files already present are skipped (huggingface_hub resumes partial downloads).
"""

import argparse
from pathlib import Path

from huggingface_hub import hf_hub_download

ROOT = Path(__file__).resolve().parent.parent
MODELS_DIR = ROOT / "models"

# name -> (repo, filename)
MODELS = {
    "qwen3.5-0.8b-q4": ("unsloth/Qwen3.5-0.8B-GGUF", "Qwen3.5-0.8B-Q4_K_M.gguf"),
    "qwen3.5-0.8b-q8": ("unsloth/Qwen3.5-0.8B-GGUF", "Qwen3.5-0.8B-Q8_0.gguf"),
    "qwen3.5-2b-q4": ("unsloth/Qwen3.5-2B-GGUF", "Qwen3.5-2B-Q4_K_M.gguf"),
    "qwen3.5-2b-q8": ("unsloth/Qwen3.5-2B-GGUF", "Qwen3.5-2B-Q8_0.gguf"),
    "qwen3.5-4b-q4": ("unsloth/Qwen3.5-4B-GGUF", "Qwen3.5-4B-Q4_K_M.gguf"),
    "qwen3.5-4b-q8": ("unsloth/Qwen3.5-4B-GGUF", "Qwen3.5-4B-Q8_0.gguf"),
    "minicpm5-1b-q4": ("openbmb/MiniCPM5-1B-GGUF", "MiniCPM5-1B-Q4_K_M.gguf"),
    "minicpm5-1b-q8": ("openbmb/MiniCPM5-1B-GGUF", "MiniCPM5-1B-Q8_0.gguf"),
    "minicpm5-2b-q4": ("openbmb/MiniCPM5-2B-GGUF", "MiniCPM5-2B-Q4_K_M.gguf"),
    "minicpm5-2b-q8": ("openbmb/MiniCPM5-2B-GGUF", "MiniCPM5-2B-Q8_0.gguf"),
    "gemma4-e2b-q4": ("unsloth/gemma-4-E2B-it-GGUF", "gemma-4-E2B-it-Q4_K_M.gguf"),
    "gemma4-e2b-q8": ("unsloth/gemma-4-E2B-it-GGUF", "gemma-4-E2B-it-Q8_0.gguf"),
    "gemma4-e4b-q4": ("unsloth/gemma-4-E4B-it-GGUF", "gemma-4-E4B-it-Q4_K_M.gguf"),
    "gemma4-e4b-q8": ("unsloth/gemma-4-E4B-it-GGUF", "gemma-4-E4B-it-Q8_0.gguf"),
    "qwen3.5-9b-q8": ("unsloth/Qwen3.5-9B-GGUF", "Qwen3.5-9B-Q8_0.gguf"),
}


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("names", nargs="*", help=f"subset of: {', '.join(MODELS)}")
    args = parser.parse_args()
    unknown = [n for n in args.names if n not in MODELS]
    if unknown:
        parser.error(f"unknown models: {unknown}")
    MODELS_DIR.mkdir(exist_ok=True)
    for name in args.names or MODELS:
        repo, filename = MODELS[name]
        path = hf_hub_download(repo, filename, local_dir=MODELS_DIR)
        print(f"{name}: {path}", flush=True)


if __name__ == "__main__":
    main()
