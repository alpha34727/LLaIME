<#
.SYNOPSIS
  Downloads the pinned ONNX Runtime release (CPU, x64) into build/onnxruntime for the engine's
  Laya ranker. src/Engine/CMakeLists.txt picks it up from there.
#>
$ErrorActionPreference = 'Stop'

$version = '1.30.0'
$sha256 = 'c6ba983baf5681af108599675d2a89c2d145512d02de28aed0bff177cd0ba949'
$name = "onnxruntime-win-x64-$version"
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
