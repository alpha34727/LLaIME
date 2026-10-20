<#
.SYNOPSIS
  Downloads the pinned CUDA Toolkit components llama.cpp's CUDA backend needs, without the
  installer, into build\cuda\<version> (for CI; a machine with the toolkit installed does not
  need this).
.DESCRIPTION
  Uses NVIDIA's redistributable archives (https://developer.download.nvidia.com/compute/cuda/redist/),
  checks each against the SHA-256 in the release's manifest, and merges them into one toolkit
  folder. Prints the folder; set CUDA_PATH to it for scripts\build-llama.ps1.
#>
param([string] $Version = '13.4.3')

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$repo = Split-Path -Parent $PSScriptRoot
$dest = Join-Path $repo "build\cuda\$Version"
$base = 'https://developer.download.nvidia.com/compute/cuda/redist'
# nvcc and its pieces, the runtime, cuBLAS, and the CCCL headers (CUB, Thrust).
$components = 'cuda_nvcc', 'cuda_crt', 'libnvvm', 'cuda_cudart', 'libcublas', 'cccl', 'cuda_nvrtc'

if (Test-Path (Join-Path $dest 'bin\nvcc.exe')) {
  Write-Host "CUDA $Version already in $dest"
  return $dest
}
$manifest = Invoke-RestMethod "$base/redistrib_$Version.json"
$work = Join-Path $repo "build\cuda\download"
New-Item -ItemType Directory -Force $work, $dest | Out-Null
foreach ($name in $components) {
  $entry = $manifest.$name.'windows-x86_64'
  if (-not $entry) { throw "CUDA $Version has no Windows archive for $name" }
  $zip = Join-Path $work (Split-Path $entry.relative_path -Leaf)
  Invoke-WebRequest "$base/$($entry.relative_path)" -OutFile $zip
  $actual = (Get-FileHash -Algorithm SHA256 $zip).Hash.ToLowerInvariant()
  if ($actual -ne $entry.sha256) { throw "${name}: sha256 $actual does not match $($entry.sha256)" }
  $unpacked = Join-Path $work $name
  Expand-Archive $zip -DestinationPath $unpacked -Force
  # Each archive holds one folder (<name>-windows-x86_64-<version>-archive) with bin, include, lib...
  $root = Get-ChildItem $unpacked -Directory | Select-Object -First 1
  Copy-Item (Join-Path $root.FullName '*') $dest -Recurse -Force
  Write-Host "$name $($manifest.$name.version)"
}
Remove-Item $work -Recurse -Force
Write-Host "CUDA $Version in $dest"
return $dest
