"""Score candidate sentences with a Laya checkpoint and evaluate it like LLaIME.Eval (stage 4b).

  score     Laya log-probabilities for every case of a cases JSONL (build_ranker_data.py),
            written as TSV: id, then option|logprob in the case's option order.
  evaluate  Sentence accuracy over an LLaIME.Eval dump: Laya alone, Laya + lexicon, and
            Laya + lexicon + the dump's LM scores, over a grid of weights. Rows the cases left
            out (true sentence not in the N best, or one candidate) count as in the dump.

Run with USE_TF=0 (see the Laya model card).
"""

import argparse
import json
import math
import time
from pathlib import Path

import numpy as np


def score(args: argparse.Namespace) -> None:
    import laya
    from laya import agent as agent_module

    agent = laya.load(str(args.checkpoint), device=args.device)
    raw: dict[int, np.ndarray] = {}

    # Probabilities in the public answer are rounded to 4 places; keep the unrounded,
    # temperature-free option logits instead.
    original = agent._decode_answers

    def decode(logits, act, items, ids, internal, offset, lang=None):
        rows = agent_module._option_logits(logits, items, offset)
        for j, qid in enumerate(ids):
            row = np.asarray(rows[j], dtype=np.float64)
            p = row - row.max()
            raw[len(raw)] = (p - np.log(np.exp(p).sum()),
                             internal[qid].get("option_order"))
        return original(logits, act, items, ids, internal, offset, lang)

    agent._decode_answers = decode
    cases = [json.loads(line) for line in args.cases.read_text(encoding="utf-8").splitlines()]
    timings = []
    with args.output.open("w", encoding="utf-8", newline="\n") as f:
        for case in cases:
            raw.clear()
            start = time.perf_counter()
            agent.predict(case["state"], case["questions"], max_len=args.max_len)
            timings.append((time.perf_counter() - start) * 1000)
            logp, order = raw[0]
            if order is not None:  # slot order -> option order
                unpermuted = np.empty_like(logp)
                unpermuted[np.asarray(order)] = logp
                logp = unpermuted
            options = list(next(iter(case["questions"].values()))["criteria"])
            f.write(case["id"] + "\t" + "\t".join(
                f"{o}|{v:.5f}" for o, v in zip(options, logp)) + "\n")
    timings.sort()
    print(f"{len(cases)} cases; latency ms p50 {timings[len(timings) // 2]:.1f} "
          f"p95 {timings[int(len(timings) * 0.95)]:.1f}")


def evaluate(args: argparse.Namespace) -> None:
    laya_scores = {}
    for line in args.scores.read_text(encoding="utf-8").splitlines():
        parts = line.split("\t")
        laya_scores[parts[0]] = {o: float(v) for o, v in (p.rsplit("|", 1) for p in parts[1:])}
    rows = []
    for line in args.dump.read_text(encoding="utf-8").splitlines():
        parts = line.split("\t")
        cands = [c.split("|") for c in parts[4:]]
        rows.append((parts[1], [(c[0], float(c[1]) * math.log(10), float(c[2])) for c in cands],
                     laya_scores.get(parts[0], {})))

    def accuracy(fn) -> float:
        correct = 0
        for truth, cands, ls in rows:
            if cands:
                correct += max(cands, key=lambda c: fn(c, ls))[0] == truth
        return 100 * correct / len(rows)

    print(f"rows {len(rows)}, scored {sum(bool(r[2]) for r in rows)}")
    print(f"lexicon only        {accuracy(lambda c, ls: c[1]):.1f}%")
    print(f"LM + 0.75 lexicon   {accuracy(lambda c, ls: c[2] + 0.75 * c[1]):.1f}%")
    print(f"Laya only           {accuracy(lambda c, ls: ls.get(c[0], -99)):.1f}%")
    for scale in (0.5, 1.0, 2.0, 4.0):
        line = " ".join(
            f"w{w:.2f} {accuracy(lambda c, ls: scale * ls.get(c[0], -99) + w * c[1]):.1f}%"
            for w in (0.0, 0.1, 0.25, 0.5, 1.0))
        print(f"Laya x{scale:<3} + w lexicon: {line}")
    for scale in (0.5, 1.0, 2.0):
        line = " ".join(
            f"w{w:.2f} {accuracy(lambda c, ls: c[2] + scale * ls.get(c[0], -99) + w * c[1]):.1f}%"
            for w in (0.25, 0.5, 0.75))
        print(f"LM + Laya x{scale:<3} + w lexicon: {line}")


def main() -> None:
    parser = argparse.ArgumentParser()
    sub = parser.add_subparsers(dest="step", required=True)
    s = sub.add_parser("score")
    s.add_argument("output", type=Path)
    s.add_argument("--checkpoint", default="convaiinnovations/laya-multilingual")
    s.add_argument("--cases", type=Path, required=True)
    s.add_argument("--device", default=None)
    s.add_argument("--max-len", type=int, default=1024, help="token budget per case")
    e = sub.add_parser("evaluate")
    e.add_argument("--scores", type=Path, required=True)
    e.add_argument("--dump", type=Path, required=True)
    args = parser.parse_args()
    score(args) if args.step == "score" else evaluate(args)


if __name__ == "__main__":
    main()
