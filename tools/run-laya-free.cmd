@echo off
set USE_TF=0
set PYTHONWARNINGS=ignore
set HF_HUB_DISABLE_PROGRESS_BARS=1
set PYTHONIOENCODING=utf-8
cd /d %~dp0
uv run laya-train --data ../data/build/free/train.jsonl --base convaiinnovations/laya-multilingual --out ../data/build/free/laya-free --loss soft-ce --shuffle-options --head-max-len 512 --max-len 768 --epochs 4 > ..\data\build\free\train-laya.log 2>&1
echo done >> ..\data\build\free\train-laya.log
