<#
.SYNOPSIS
  Logs how a program's processes end (exit code, time), to tell crashes apart.
.DESCRIPTION
  Watches every process of the given executable (default: claude.exe, an Electron app with a
  main process and child processes such as the GPU process). When one exits, a line is added
  to the log with its role, exit code (and what the code means), whether LLaIME.Tip.dll was
  loaded in it, and whether Windows reset the display driver (TDR, event 141/117) within the
  last two minutes. Runs until stopped (Ctrl+C); no administrator rights needed.
.PARAMETER Name
  Executable to watch, without .exe.
.PARAMETER PathMatch
  Only processes whose executable path matches this regex (default: the Claude desktop app in
  WindowsApps, not the Claude Code command line, which is also claude.exe).
.PARAMETER Log
  Log file (default: %LOCALAPPDATA%\LLaIME\crash-watch.log).
#>
param(
  [string] $Name = 'claude',
  [string] $PathMatch = '\\WindowsApps\\Claude_',
  [string] $Log = (Join-Path $env:LOCALAPPDATA 'LLaIME\crash-watch.log')
)

$ErrorActionPreference = 'Continue'
New-Item -ItemType Directory -Force (Split-Path $Log) | Out-Null

$codes = @{
  0           = 'normal exit'
  1           = 'exit code 1 (often a normal quit for Electron)'
  -1073741819 = 'ACCESS_VIOLATION 0xC0000005 (crash)'
  -1073740791 = 'STACK_BUFFER_OVERRUN / fail-fast 0xC0000409 (crash)'
  -1073740940 = 'HEAP_CORRUPTION 0xC0000374 (crash)'
  -2147483645 = 'BREAKPOINT 0x80000003 (crash or debug break)'
  -1073741571 = 'STACK_OVERFLOW 0xC00000FD (crash)'
  -1073741510 = 'CONTROL_C_EXIT 0xC000013A (closed by system/console)'
  -1073741205 = 'STATUS_FATAL_USER_CALLBACK_EXCEPTION 0xC000041D (crash in a callback)'
  2170        = '0x87A: graphics device lost (display driver reset)'
  -805306369  = '0xCFFFFFFF: Electron renderer killed (out of memory or killed)'
}

function Write-Log([string] $text) {
  $line = '{0:yyyy-MM-dd HH:mm:ss}  {1}' -f (Get-Date), $text
  Add-Content -Path $Log -Value $line -Encoding UTF8
  Write-Host $line
}

function Get-Role([int] $id) {
  $cmd = (Get-CimInstance Win32_Process -Filter "ProcessId=$id" -ErrorAction SilentlyContinue).CommandLine
  if (-not $cmd) { return 'unknown' }
  if ($cmd -match '--type=([\w-]+)') { return $Matches[1] }
  return 'main'
}

function Recent-Tdr {
  $events = Get-WinEvent -FilterHashtable @{LogName = 'Application'; Id = 1001; StartTime = (Get-Date).AddMinutes(-2)} -ErrorAction SilentlyContinue |
    Where-Object { $_.Message -match 'LiveKernelEvent' -and $_.Message -match 'P1: (141|117)' }
  if ($events) { return 'yes, at ' + (($events | ForEach-Object { '{0:HH:mm:ss}' -f $_.TimeCreated }) -join ', ') }
  return 'no'
}

$watched = @{}  # pid -> @{Process; Role; Tip}
Write-Log "watching $Name.exe (log: $Log)"
while ($true) {
  foreach ($p in Get-Process -Name $Name -ErrorAction SilentlyContinue) {
    if ($watched.ContainsKey($p.Id)) { continue }
    if ($PathMatch -and $p.Path -notmatch $PathMatch) { $watched[$p.Id] = $null; continue }
    try { $null = $p.Handle } catch { continue }  # keep a handle so the exit code stays readable
    $tip = $false
    try { $tip = [bool]($p.Modules | Where-Object ModuleName -eq 'LLaIME.Tip.dll') } catch {}
    $watched[$p.Id] = @{ Process = $p; Role = (Get-Role $p.Id); Tip = $tip }
    if ($watched[$p.Id].Role -eq 'main') { Write-Log ("started  pid {0} (main)" -f $p.Id) }
  }
  foreach ($id in @($watched.Keys)) {
    $w = $watched[$id]
    if (-not $w) {
      if (-not (Get-Process -Id $id -ErrorAction SilentlyContinue)) { $watched.Remove($id) }
      continue
    }
    $p = $w.Process
    # LLaIME loads when the user first types in that process; look again until it is there.
    if (-not $w.Tip -and -not $p.HasExited) {
      try { $w.Tip = [bool]($p.Modules | Where-Object ModuleName -eq 'LLaIME.Tip.dll') } catch {}
    }
    if (-not $p.HasExited) { continue }
    $code = $p.ExitCode
    $meaning = if ($codes.ContainsKey($code)) { $codes[$code] } else { 'exit code 0x{0:X8}' -f $code }
    if ($w.Role -eq 'main' -or $code -ne 0) {
      Write-Log ("exited   pid {0} ({1}): {2}; LLaIME loaded: {3}; display driver reset in last 2 min: {4}" -f
        $id, $w.Role, $meaning, $(if ($w.Tip) { 'yes' } else { 'no' }), (Recent-Tdr))
    }
    $watched.Remove($id)
  }
  Start-Sleep -Seconds 2
}
