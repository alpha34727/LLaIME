<#
.SYNOPSIS
  Installs the locally built TIP DLLs into Program Files and registers them (dev use).
.DESCRIPTION
  Copies build\x64 and build\x86 LLaIME.Tip.dll to "C:\Program Files\LLaIME\{x64,x86}" and
  registers them with regsvr32. A DLL that is loaded by running apps cannot be overwritten,
  so the old file is renamed aside first. Also installs the lexicon layers (data\lexicons), the
  engine, and the settings app (settings\, built here first) with a Start menu shortcut.
  Self-elevates via UAC.
.PARAMETER Configuration
  Debug or Release. Defaults to whichever x64 TIP build is newer.
.PARAMETER Model
  GGUF file under models\ that the engine loads (written to %LOCALAPPDATA%\LLaIME\engine.ini
  unless that file already exists).
.PARAMETER Uninstall
  Unregister and remove the installed DLLs.
#>
param(
  [ValidateSet('', 'Debug', 'Release')] [string] $Configuration = '',
  [string] $Model = 'Qwen3.5-2B-Q4_K_M.gguf',
  [switch] $Uninstall
)

$ErrorActionPreference = 'Stop'

if (-not $Configuration) {
  $root = Split-Path -Parent $PSScriptRoot
  $Configuration = 'Debug', 'Release' |
    Where-Object { Test-Path "$root/build/x64/src/Tip/$_/LLaIME.Tip.dll" } |
    Sort-Object { (Get-Item "$root/build/x64/src/Tip/$_/LLaIME.Tip.dll").LastWriteTime } |
    Select-Object -Last 1
  if (-not $Configuration) { throw 'No TIP build found under build/x64/src/Tip' }
  Write-Host "Installing the $Configuration build"
}

$principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
  # The settings app is built before elevating, as the user (NuGet cache, no admin files).
  if (-not $Uninstall) {
    $app = Join-Path (Split-Path -Parent $PSScriptRoot) 'src\Settings\App'
    Write-Host 'Building the settings app'
    & dotnet build $app -c Release --nologo -v quiet
    if ($LASTEXITCODE -ne 0) { throw 'Settings app build failed' }
  }
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
  Get-Process LLaIME.Settings -ErrorAction SilentlyContinue | Stop-Process -Force
  Remove-Item (Join-Path $env:ProgramData 'Microsoft\Windows\Start Menu\Programs\LLaIME 設定.lnk') -ErrorAction SilentlyContinue
  Remove-Item $installRoot -Recurse -Force -ErrorAction SilentlyContinue
} else {
  # Lexicon layers, one .bin + .ini per source. Running apps map them, so files in use are
  # renamed aside rather than overwritten.
  $layers = Join-Path $repo 'data\build\lexicons'
  if (-not (Test-Path (Join-Path $layers 'mcbopomofo.bin'))) {
    throw "Missing $layers (run: cd tools; uv run python prepare_lexicon.py)"
  }
  $lexDir = Join-Path $installRoot 'data\lexicons'
  New-Item -ItemType Directory -Force $lexDir | Out-Null
  Get-ChildItem $lexDir -Filter '*.old*' -ErrorAction SilentlyContinue |
    ForEach-Object { try { Remove-Item $_.FullName -Force -ErrorAction Stop } catch {} }
  $stamp = [DateTime]::Now.ToString('yyyyMMddHHmmss')
  foreach ($file in Get-ChildItem $layers -Include '*.bin', '*.ini' -File -Recurse) {
    $dest = Join-Path $lexDir $file.Name
    if ((Test-Path $dest) -and $file.Extension -eq '.bin') { Rename-Item $dest ($file.Name + ".old$stamp") }
    Copy-Item $file.FullName $dest -Force
  }
  # The single-file lexicon of earlier versions.
  foreach ($old in Get-ChildItem (Join-Path $installRoot 'data') -Filter 'lexicon.bin*' -File -ErrorAction SilentlyContinue) {
    try { Remove-Item $old.FullName -Force -ErrorAction Stop } catch { Rename-Item $old.FullName ($old.Name + ".old$stamp") -ErrorAction SilentlyContinue }
  }
  Write-Host "Installed lexicon layers to $lexDir"

  # Settings app (self-contained WinUI 3 build with the native bridge).
  $appOut = Get-ChildItem (Join-Path $repo 'src\Settings\App\bin\Release') -Recurse -Filter 'LLaIME.Settings.exe' |
    Sort-Object LastWriteTime | Select-Object -Last 1
  if ($appOut) {
    $settingsDir = Join-Path $installRoot 'settings'
    if (Test-Path $settingsDir) {
      Get-Process LLaIME.Settings -ErrorAction SilentlyContinue | Stop-Process -Force
      Start-Sleep -Milliseconds 300
      Remove-Item $settingsDir -Recurse -Force
    }
    Copy-Item $appOut.DirectoryName $settingsDir -Recurse
    $shortcut = Join-Path $env:ProgramData 'Microsoft\Windows\Start Menu\Programs\LLaIME 設定.lnk'
    $shell = New-Object -ComObject WScript.Shell
    $link = $shell.CreateShortcut($shortcut)
    $link.TargetPath = Join-Path $settingsDir 'LLaIME.Settings.exe'
    $link.WorkingDirectory = $settingsDir
    $link.Description = 'LLaIME 注音輸入法設定'
    $link.Save()
    Write-Host "Installed the settings app to $settingsDir (Start menu: LLaIME 設定)"
  } else {
    Write-Warning 'Settings app not built (src\Settings\App)'
  }

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
      Set-Content -Path $ini -Encoding Unicode -Value "[model]`r`nranker=auto`r`npath=$modelPath`r`ndevice=auto"
      Write-Host "Wrote $ini (model: $modelPath)"
    }
    # CPU mode (ranker=laya, or auto without a GPU) needs the Laya model directory.
    $layaDir = Join-Path $repo 'models/laya-zh'
    if ((Test-Path (Join-Path $layaDir 'laya.int8.onnx')) -and
        -not (Select-String -Path $ini -Pattern '^\[laya\]' -Quiet)) {
      Add-Content -Path $ini -Encoding Unicode -Value "[laya]`r`npath=$((Resolve-Path $layaDir).Path)`r`nthreads=0"
      Write-Host "Added the Laya model ($layaDir) to $ini"
    }
  } else {
    Write-Warning "Engine not built ($engineSrc); typing works with the lexicon only."
  }
}
