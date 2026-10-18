<#
.SYNOPSIS
  Installs, checks, uninstalls, checks and reinstalls an LLaIME MSI (self-elevates once).
.DESCRIPTION
  Replaces a development install (scripts/dev-install.ps1) first: unregisters its TIPs and
  renames DLLs that running apps hold aside, so the MSI does not have to replace files in use.
  After each step it checks the files, the COM / TSF registration, the Start menu shortcut and
  whether LLaIME 注音 is in the user's keyboard list. Results go to build\installer\test.log,
  Windows Installer logs next to it.
.PARAMETER Msi
  The MSI to test.
.PARAMETER FinalMsi
  The MSI to leave installed at the end (default: -Msi).
#>
param(
  [Parameter(Mandatory)] [string] $Msi,
  [string] $FinalMsi = ''
)

$ErrorActionPreference = 'Stop'
$Msi = (Resolve-Path $Msi).Path
if (-not $FinalMsi) { $FinalMsi = $Msi }
$FinalMsi = (Resolve-Path $FinalMsi).Path

$principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
  $argList = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$PSCommandPath`"",
               '-Msi', "`"$Msi`"", '-FinalMsi', "`"$FinalMsi`"")
  $proc = Start-Process powershell -Verb RunAs -ArgumentList $argList -Wait -PassThru
  exit $proc.ExitCode
}

$repo = Split-Path -Parent $PSScriptRoot
$logDir = Join-Path $repo 'build\installer'
$log = Join-Path $logDir 'test.log'
Set-Content $log "LLaIME installer test $(Get-Date -Format s)" -Encoding UTF8
function Log([string] $line) { Add-Content $log $line -Encoding UTF8 }

$root = Join-Path $env:ProgramFiles 'LLaIME'
$clsid = '{5B72F657-F087-4472-A827-7FD8EB608DE6}'
$tip = "0404:$clsid{671F61EB-E460-42B0-8B3A-C90CB1488716}"
$shortcut = Join-Path $env:ProgramData 'Microsoft\Windows\Start Menu\Programs\LLaIME 設定.lnk'
$failures = 0

function Check([string] $what, [bool] $ok) {
  Log ("  {0} {1}" -f $(if ($ok) { 'ok  ' } else { 'FAIL' }), $what)
  if (-not $ok) { $script:failures++ }
}

function Test-Installed([bool] $expected) {
  $files = 'x64\LLaIME.Tip.dll', 'x86\LLaIME.Tip.dll', 'engine\LLaIME.Engine.exe', 'engine\vcruntime140.dll',
           'settings\LLaIME.Settings.exe', 'settings\LLaIME.SettingsNative.dll', 'data\lexicons\mcbopomofo.bin',
           'models\catalog.json', 'setup\LLaIME.InputSetup.exe', 'licenses\THIRD-PARTY-NOTICES.txt'
  foreach ($f in $files) {
    $exists = Test-Path (Join-Path $root $f)
    if ($expected) {
      Check "file $f" $exists
    } elseif ($exists -and $f -like '*Tip.dll') {
      # An app may still have it loaded: Windows Installer deletes it at the next restart.
      Log "  note file $f is still there (in use; removed at restart)"
    } else {
      Check "no file $f" (-not $exists)
    }
  }
  $com64 = (Get-ItemProperty "HKLM:\SOFTWARE\Classes\CLSID\$clsid\InprocServer32" -ErrorAction SilentlyContinue).'(default)'
  $com32 = (Get-ItemProperty "HKLM:\SOFTWARE\WOW6432Node\Classes\CLSID\$clsid\InprocServer32" -ErrorAction SilentlyContinue).'(default)'
  $tsf = Test-Path "HKLM:\SOFTWARE\Microsoft\CTF\TIP\$clsid"
  if ($expected) {
    Check "COM x64 -> $com64" ($com64 -eq (Join-Path $root 'x64\LLaIME.Tip.dll'))
    Check "COM x86 -> $com32" ($com32 -eq (Join-Path $root 'x86\LLaIME.Tip.dll'))
    Check 'TSF profile registered' $tsf
    Check 'Start menu shortcut' (Test-Path -LiteralPath $shortcut)
  } else {
    Check 'COM x64 unregistered' (-not $com64)
    Check 'COM x86 unregistered' (-not $com32)
    Check 'TSF profile unregistered' (-not $tsf)
    Check 'no Start menu shortcut' (-not (Test-Path -LiteralPath $shortcut))
  }
  $profile = Get-ItemProperty 'HKCU:\Control Panel\International\User Profile\zh-Hant-TW' -ErrorAction SilentlyContinue
  $inList = $profile -and ($profile.PSObject.Properties.Name -contains $tip)
  Check $(if ($expected) { "in the user's keyboard list" } else { "not in the user's keyboard list" }) ($inList -eq $expected)
}

function Invoke-Msi([string] $verb, [string] $package, [string] $name) {
  $msiLog = Join-Path $logDir "$name.log"
  $p = Start-Process msiexec.exe -ArgumentList "/$verb", "`"$package`"", '/qn', '/norestart', '/l*v', "`"$msiLog`"" -Wait -PassThru
  Log "$name ($([IO.Path]::GetFileName($package))): exit $($p.ExitCode)$(if ($p.ExitCode -eq 3010) { ' (restart needed to finish)' })"
  if ($p.ExitCode -ne 0 -and $p.ExitCode -ne 3010) { $script:failures++ }
}

# Replace a development install.
$installed = Get-CimInstance Win32_Product -Filter "Name = 'LLaIME 注音輸入法'" -ErrorAction SilentlyContinue
if (-not $installed -and (Test-Path $root)) {
  Log 'Removing the development install'
  Get-Process LLaIME.Engine, LLaIME.Settings -ErrorAction SilentlyContinue | Stop-Process -Force
  foreach ($arch in @(@{ Dir = 'x64'; RegSvr = "$env:WINDIR\System32\regsvr32.exe" },
                      @{ Dir = 'x86'; RegSvr = "$env:WINDIR\SysWOW64\regsvr32.exe" })) {
    $dll = Join-Path $root "$($arch.Dir)\LLaIME.Tip.dll"
    if (Test-Path $dll) {
      Start-Process $arch.RegSvr -ArgumentList '/s', '/u', "`"$dll`"" -Wait
      Rename-Item $dll ("LLaIME.Tip.dll.old" + [DateTime]::Now.ToString('yyyyMMddHHmmss'))
    }
  }
  Get-ChildItem $root -Recurse -File | Where-Object { $_.Name -notlike '*.old*' } |
    ForEach-Object { try { Remove-Item $_.FullName -Force -ErrorAction Stop } catch { Rename-Item $_.FullName ($_.Name + '.old') } }
  Remove-Item (Join-Path $env:ProgramData 'Microsoft\Windows\Start Menu\Programs\LLaIME 設定.lnk') -ErrorAction SilentlyContinue
}

Invoke-Msi 'i' $Msi 'install'
Test-Installed $true
Invoke-Msi 'x' $Msi 'uninstall'
Test-Installed $false
Invoke-Msi 'i' $FinalMsi 'reinstall'
Test-Installed $true

Log "failures: $failures"
exit $failures
