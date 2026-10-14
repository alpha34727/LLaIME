"""Fetch the dictionary sources at pinned versions and build data/build/lexicon.bin.

  * McBopomofo (MIT, data/raw/McBopomofo/LICENSE.txt): the main phrase table and scores.
  * libchewing-data (LGPL-2.1-or-later): phrases with frequencies missing from McBopomofo.

See supplement_lexicon.py for how the extra phrases are merged and scored.
"""

import os
import subprocess
import sys
from pathlib import Path

MCBOPOMOFO_REPO = "https://github.com/openvanilla/McBopomofo.git"
MCBOPOMOFO_COMMIT = "be6564acad6c4d3265c34a2e1a872d80f9db6068"
CHEWING_REPO = "https://github.com/chewing/libchewing-data.git"
CHEWING_COMMIT = "c44e81aef24b06f1509f19e1be54c99812d0c43f"

ROOT = Path(__file__).resolve().parent.parent
RAW = ROOT / "data" / "raw"
MCBOPOMOFO = RAW / "McBopomofo"
DATA = MCBOPOMOFO / "Source" / "Data"
CHEWING = RAW / "libchewing-data"
BUILD = ROOT / "data" / "build"
OUTPUT = BUILD / "lexicon.bin"


def run(args: list[str], cwd: Path) -> None:
    env = dict(os.environ, PYTHONUTF8="1")
    subprocess.run(args, cwd=cwd, check=True, env=env)


def fetch_git(repo: str, commit: str, checkout: Path, sparse: str | None = None) -> None:
    if not checkout.exists():
        checkout.parent.mkdir(parents=True, exist_ok=True)
        clone = ["git", "clone", "--filter=blob:none", "--no-checkout", repo, str(checkout)]
        if sparse:
            clone.insert(3, "--sparse")
        run(clone, ROOT)
        if sparse:
            run(["git", "sparse-checkout", "set", sparse], checkout)
    run(["git", "fetch", "--depth", "1", "origin", commit], checkout)
    run(["git", "checkout", "--quiet", commit], checkout)


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
    fetch_git(MCBOPOMOFO_REPO, MCBOPOMOFO_COMMIT, MCBOPOMOFO, sparse="Source/Data")
    fetch_git(CHEWING_REPO, CHEWING_COMMIT, CHEWING)
    compile_mcbopomofo()
    supplement = BUILD / "supplement.txt"
    tools = ROOT / "tools"
    run([sys.executable, str(tools / "supplement_lexicon.py"), "--mcbopomofo",
         str(DATA / "data.txt"), "--chewing",
         str(CHEWING / "dict" / "chewing" / "tsi.csv"), "--output", str(supplement)], ROOT)
    run([sys.executable, str(tools / "build_lexicon.py"), str(DATA / "data.txt"),
         str(supplement), "--output", str(OUTPUT)], ROOT)


if __name__ == "__main__":
    main()
