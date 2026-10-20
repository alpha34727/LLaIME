@echo off
rem Qwen3.5-0.8B LoRA on the freely licensed corpus, the same way as run-lora-free.cmd, then GGUF
rem and evaluation of the base (same conversion) and LoRA models with the MOE-free lexicon.
set PYTHONWARNINGS=ignore
set HF_HUB_DISABLE_PROGRESS_BARS=1
set TRANSFORMERS_VERBOSITY=error
set PYTHONIOENCODING=utf-8
cd /d C:\Users\Johnsou\Desktop\LLaIME
set LOG=data\build\free\run-lora-0.8b.log
echo [train] %date% %time% > %LOG%
tools\.venv\Scripts\python.exe -X utf8 tools\lora_qwen.py models\hf\Qwen3.5-0.8B data\build\free\lm_corpus.jsonl data\build\free\qwen3.5-0.8b-tw-free >> %LOG% 2>&1
if errorlevel 1 goto failed
echo [convert] %date% %time% >> %LOG%
tools\.venv\Scripts\python.exe -X utf8 third_party\llama.cpp\convert_hf_to_gguf.py data\build\free\qwen3.5-0.8b-tw-free --outtype bf16 --outfile models\tw-free\Qwen3.5-0.8B-TW-IME-BF16.gguf > data\build\free\convert-0.8b.log 2>&1
if errorlevel 1 goto failed
build\llama\Release\install\bin\llama-quantize.exe models\tw-free\Qwen3.5-0.8B-TW-IME-BF16.gguf models\tw-free\Qwen3.5-0.8B-TW-IME-Q4_K_M.gguf Q4_K_M > data\build\free\quantize-0.8b.log 2>&1
if errorlevel 1 goto failed
tools\.venv\Scripts\python.exe -X utf8 third_party\llama.cpp\convert_hf_to_gguf.py models\hf\Qwen3.5-0.8B --outtype bf16 --outfile models\tw-free\Qwen3.5-0.8B-base-BF16.gguf > data\build\free\convert-0.8b-base.log 2>&1
if errorlevel 1 goto failed
build\llama\Release\install\bin\llama-quantize.exe models\tw-free\Qwen3.5-0.8B-base-BF16.gguf models\tw-free\Qwen3.5-0.8B-base-Q4_K_M.gguf Q4_K_M >> data\build\free\quantize-0.8b.log 2>&1
if errorlevel 1 goto failed
for %%m in (models\tw-free\Qwen3.5-0.8B-base-Q4_K_M.gguf models\tw-free\Qwen3.5-0.8B-TW-IME-Q4_K_M.gguf) do (
  for %%d in (commonvoice_dev commonvoice_test taiwan_usage ly_test press_test wikipedia_test) do (
    echo [eval] %%~nm %%d >> %LOG%
    build\x64\src\Engine\Release\LLaIME.Eval.exe --lexicon data\build\lexicons-free --data tests\eval\%%d.tsv --model %%m --device CUDA0 --dump data\build\free\dump_%%~nm_%%d.tsv 2>nul | findstr /R /C:"lexicon" /C:"oracle" /C:"penalty 4" /C:"latency" >> %LOG%
  )
)
echo [done] %date% %time% >> %LOG%
exit /b 0
:failed
echo [failed] %date% %time% >> %LOG%
