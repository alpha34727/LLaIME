<#
.SYNOPSIS
  Downloads the pinned ONNX Runtime release (x64, with the CUDA 13 execution provider) into
  build/onnxruntime for the engine's Laya ranker. src/Engine/CMakeLists.txt picks it up from there.
  The CUDA provider needs only cuBLAS 13, which the engine already ships for llama.cpp; without
  an NVIDIA GPU, Laya runs on the CPU provider in the same onnxruntime.dll.
#>
$ErrorActionPreference = 'Stop'

$version = '1.30.0'
$sha256 = '8fa4b08359af682cd605892cb59077049700b640128bb93fd2c7776cf9f55bdc'
$name = "onnxruntime-win-x64-gpu_cuda13-$version"
$url = "https://github.com/microsoft/onnxruntime/releases/download/v$version/$name.zip"

$repo = Split-Path -Parent $PSScriptRoot
$dest = Join-Path $repo 'build/onnxruntime'
if (Test-Path (Join-Path $dest "$name/lib/onnxruntime.dll")) {
  Write-Host "ONNX Runtime $version already in $dest"
  exit 0
}
New-Item -ItemType Directory -Force $dest | Out-Null
$zip = Join-Path $dest "$name.zip"
Invoke-WebRequest -Uri $url -OutFile $zip
$actual = (Get-FileHash -Algorithm SHA256 $zip).Hash.ToLowerInvariant()
if ($actual -ne $sha256) {
  Remove-Item $zip
  throw "$name.zip: sha256 $actual does not match the pinned $sha256"
}
Expand-Archive -Path $zip -DestinationPath $dest -Force
Remove-Item $zip
Write-Host "ONNX Runtime $version extracted to $dest/$name"
