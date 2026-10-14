<#
.SYNOPSIS
  Installs the locally built TIP DLLs into Program Files and registers them (dev use).
.DESCRIPTION
  Copies build\x64 and build\x86 LLaIME.Tip.dll to "C:\Program Files\LLaIME\{x64,x86}" and
  registers them with regsvr32. A DLL that is loaded by running apps cannot be overwritten,
  so the old file is renamed aside first. Self-elevates via UAC.
.PARAMETER Configuration
  Debug or Release.
.PARAMETER Model
  GGUF file under models\ that the engine loads (written to %LOCALAPPDATA%\LLaIME\engine.ini
  unless that file already exists).
.PARAMETER Uninstall
  Unregister and remove the installed DLLs.
#>
param(
  [ValidateSet('Debug', 'Release')] [string] $Configuration = 'Debug',
  [string] $Model = 'Qwen3.5-2B-Q4_K_M.gguf',
  [switch] $Uninstall
)

$ErrorActionPreference = 'Stop'

$principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
  $argList = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$PSCommandPath`"", '-Configuration', $Configuration, '-Model', $Model)
  if ($Uninstall) { $argList += '-Uninstall' }
  $proc = Start-Process powershell -Verb RunAs -ArgumentList $argList -Wait -PassThru
  exit $proc.ExitCode
}

$repo = Split-Path -Parent $PSScriptRoot
$installRoot = Join-Path $env:ProgramFiles 'LLaIME'
# The engine holds its files open; stop it before replacing or removing them.
Get-Process LLaIME.Engine -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 300
$targets = @(
  @{ Arch = 'x64'; RegSvr = "$env:WINDIR\System32\regsvr32.exe" },
  @{ Arch = 'x86'; RegSvr = "$env:WINDIR\SysWOW64\regsvr32.exe" }
)

foreach ($t in $targets) {
  $dir = Join-Path $installRoot $t.Arch
  $dll = Join-Path $dir 'LLaIME.Tip.dll'

  if ($Uninstall) {
    if (Test-Path $dll) {
      & $t.RegSvr /s /u $dll
      Write-Host "Unregistered $dll"
    }
    continue
  }

  $src = Join-Path $repo "build\$($t.Arch)\src\Tip\$Configuration\LLaIME.Tip.dll"
  if (-not (Test-Path $src)) { throw "Missing build output: $src" }
  New-Item -ItemType Directory -Force $dir | Out-Null

  # Clean up DLLs renamed aside by earlier installs (succeeds once no process has them loaded).
  Get-ChildItem $dir -Filter 'LLaIME.Tip.dll.old*' -ErrorAction SilentlyContinue |
    ForEach-Object { try { Remove-Item $_.FullName -Force -ErrorAction Stop } catch {} }

  if (Test-Path $dll) {
    Rename-Item $dll ("LLaIME.Tip.dll.old" + [DateTime]::Now.ToString('yyyyMMddHHmmss'))
  }
  Copy-Item $src $dll
  $pdb = [IO.Path]::ChangeExtension($src, '.pdb')
  if (Test-Path $pdb) { Copy-Item $pdb $dir -Force -ErrorAction SilentlyContinue }

  $p = Start-Process $t.RegSvr -ArgumentList '/s', "`"$dll`"" -Wait -PassThru
  if ($p.ExitCode -ne 0) { throw "regsvr32 failed for $dll (exit $($p.ExitCode))" }
  Write-Host "Registered $dll"
}

if ($Uninstall) {
  Remove-Item $installRoot -Recurse -Force -ErrorAction SilentlyContinue
} else {
  $lexicon = Join-Path $repo 'data\build\lexicon.bin'
  if (-not (Test-Path $lexicon)) { throw "Missing $lexicon (run: cd tools; uv run python prepare_lexicon.py)" }
  $dataDir = Join-Path $installRoot 'data'
  New-Item -ItemType Directory -Force $dataDir | Out-Null
  $dest = Join-Path $dataDir 'lexicon.bin'
  # The lexicon is memory-mapped by running apps; rename it aside like the DLLs.
  Get-ChildItem $dataDir -Filter 'lexicon.bin.old*' -ErrorAction SilentlyContinue |
    ForEach-Object { try { Remove-Item $_.FullName -Force -ErrorAction Stop } catch {} }
  if (Test-Path $dest) { Rename-Item $dest ("lexicon.bin.old" + [DateTime]::Now.ToString('yyyyMMddHHmmss')) }
  Copy-Item $lexicon $dest
  Write-Host "Installed $dest"

  # Engine (always the Release build: it runs the model) and llama.cpp / CUDA DLLs.
  $engineSrc = Join-Path $repo 'build/x64/src/Engine/Release'
  $engineDir = Join-Path $installRoot 'engine'
  if (Test-Path (Join-Path $engineSrc 'LLaIME.Engine.exe')) {
    New-Item -ItemType Directory -Force $engineDir | Out-Null
    Copy-Item (Join-Path $engineSrc 'LLaIME.Engine.exe') $engineDir -Force
    Copy-Item (Join-Path $engineSrc '*.dll') $engineDir -Force
    Write-Host "Installed engine to $engineDir"

    $settingsDir = Join-Path $env:LOCALAPPDATA 'LLaIME'
    $ini = Join-Path $settingsDir 'engine.ini'
    if (-not (Test-Path $ini)) {
      New-Item -ItemType Directory -Force $settingsDir | Out-Null
      $modelPath = Join-Path $repo "models\$Model"
      Set-Content -Path $ini -Encoding Unicode -Value "[model]`r`npath=$modelPath`r`ndevice=auto"
      Write-Host "Wrote $ini (model: $modelPath)"
    }
  } else {
    Write-Warning "Engine not built ($engineSrc); typing works with the lexicon only."
  }
}
