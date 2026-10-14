"""Fetch McBopomofo's dictionary sources at a pinned commit and build data/build/lexicon.bin.

McBopomofo data is MIT licensed (see data/raw/McBopomofo/LICENSE.txt).
"""

import os
import subprocess
import sys
from pathlib import Path

REPO = "https://github.com/openvanilla/McBopomofo.git"
COMMIT = "be6564acad6c4d3265c34a2e1a872d80f9db6068"

ROOT = Path(__file__).resolve().parent.parent
CHECKOUT = ROOT / "data" / "raw" / "McBopomofo"
DATA = CHECKOUT / "Source" / "Data"
OUTPUT = ROOT / "data" / "build" / "lexicon.bin"


def run(args: list[str], cwd: Path) -> None:
    env = dict(os.environ, PYTHONUTF8="1")
    subprocess.run(args, cwd=cwd, check=True, env=env)


def fetch() -> None:
    if not CHECKOUT.exists():
        CHECKOUT.parent.mkdir(parents=True, exist_ok=True)
        run(["git", "clone", "--filter=blob:none", "--sparse", "--no-checkout", REPO,
             str(CHECKOUT)], ROOT)
        run(["git", "sparse-checkout", "set", "Source/Data"], CHECKOUT)
    run(["git", "fetch", "--depth", "1", "origin", COMMIT], CHECKOUT)
    run(["git", "checkout", "--quiet", COMMIT], CHECKOUT)


def compile_mcbopomofo() -> None:
    py = [sys.executable, "-m"]
    run(py + ["curation.builders.frequency_builder"], DATA)
    run(py + ["curation.compilers.main_compiler",
              "--heterophony1", "heterophony1.list", "--heterophony2", "heterophony2.list",
              "--heterophony3", "heterophony3.list", "--phrase_freq", "PhraseFreq.txt",
              "--bpmf_mappings", "BPMFMappings.txt", "--bpmf_base", "BPMFBase.txt",
              "--punctuations", "BPMFPunctuations.txt", "--symbols", "Symbols.txt",
              "--macros", "Macros.txt", "--output", "data-raw.txt"], DATA)
    run(py + ["curation.compilers.postprocess", "--input", "data-raw.txt",
              "--directive", "Postprocess.txt", "--output", "data.txt"], DATA)


def main() -> None:
    fetch()
    compile_mcbopomofo()
    run([sys.executable, str(ROOT / "tools" / "build_lexicon.py"), str(DATA / "data.txt"),
         str(OUTPUT)], ROOT)


if __name__ == "__main__":
    main()
