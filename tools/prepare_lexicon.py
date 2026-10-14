"""Fetch the dictionary sources at pinned versions and build data/build/lexicon.bin.

  * McBopomofo (MIT, data/raw/McBopomofo/LICENSE.txt): the main phrase table and scores.
  * libchewing-data (LGPL-2.1-or-later): phrases with frequencies missing from McBopomofo.
  * Ministry of Education, 重編國語辭典修訂本 (CC BY-ND 3.0 TW): headwords and readings missing
    from both. The dictionary file itself is not redistributed; it is downloaded here.

See supplement_lexicon.py for how the extra phrases are merged and scored.
"""

import hashlib
import os
import subprocess
import sys
import urllib.request
import zipfile
from pathlib import Path

MCBOPOMOFO_REPO = "https://github.com/openvanilla/McBopomofo.git"
MCBOPOMOFO_COMMIT = "be6564acad6c4d3265c34a2e1a872d80f9db6068"
CHEWING_REPO = "https://github.com/chewing/libchewing-data.git"
CHEWING_COMMIT = "c44e81aef24b06f1509f19e1be54c99812d0c43f"
MOE_URL = ("https://language.moe.gov.tw/001/Upload/Files/site_content/M0001/respub/download/"
           "dict_revised_2015_20260929.zip")
MOE_SHA256 = "65a24fff76d0fbfa078541f39761bb9f302bdf7ad6bb48be4ce1052d1b49f609"
MOE_XLSX = "dict_revised_2015_20260929.xlsx"

ROOT = Path(__file__).resolve().parent.parent
RAW = ROOT / "data" / "raw"
MCBOPOMOFO = RAW / "McBopomofo"
DATA = MCBOPOMOFO / "Source" / "Data"
CHEWING = RAW / "libchewing-data"
MOE = RAW / "moe"
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


def fetch_moe() -> Path:
    xlsx = MOE / MOE_XLSX
    if xlsx.exists():
        return xlsx
    MOE.mkdir(parents=True, exist_ok=True)
    archive = MOE / Path(MOE_URL).name
    if not archive.exists():
        urllib.request.urlretrieve(MOE_URL, archive)
    digest = hashlib.sha256(archive.read_bytes()).hexdigest()
    if digest != MOE_SHA256:
        archive.unlink()
        raise SystemExit(f"{archive.name}: sha256 {digest} does not match the pinned version")
    with zipfile.ZipFile(archive) as z:
        z.extract(MOE_XLSX, MOE)
    return xlsx


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
    moe_xlsx = fetch_moe()
    compile_mcbopomofo()
    supplement = BUILD / "supplement.txt"
    tools = ROOT / "tools"
    run([sys.executable, str(tools / "supplement_lexicon.py"), "--mcbopomofo",
         str(DATA / "data.txt"), "--moe", str(moe_xlsx), "--chewing",
         str(CHEWING / "dict" / "chewing" / "tsi.csv"), "--output", str(supplement)], ROOT)
    run([sys.executable, str(tools / "build_lexicon.py"), str(DATA / "data.txt"),
         str(supplement), "--output", str(OUTPUT)], ROOT)


if __name__ == "__main__":
    main()
