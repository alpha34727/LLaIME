<#
.SYNOPSIS
  Builds the LLaIME installer (MSI, WiX Toolset 7) into build\installer.
.DESCRIPTION
  1. Builds the Release TIPs (x64, x86), the engine, the settings bridge and helper (CMake), and
     the settings app (dotnet).
  2. Stages what is installed into build\installer\stage:
       x64\ x86\    LLaIME.Tip.dll
       engine\      LLaIME.Engine.exe and the DLLs it loads (llama.cpp, the CPU and CUDA ggml
                    backends with the CUDA runtime, ONNX Runtime, the VC++ runtime app-locally)
       settings\    the settings app (self-contained WinUI 3)
       data\lexicons\  lexicon layers (the Ministry of Education layer only with -IncludeMoe)
       models\catalog.json   the models the settings app can download (installer\models.json)
       setup\       LLaIME.InputSetup.exe
       licenses\    THIRD-PARTY-NOTICES.txt and the licenses it refers to
     Models are not included: the settings app downloads them.
  3. Builds installer\LLaIME.Installer.wixproj.
  WiX 7 is under the Open Source Maintenance Fee EULA, accepted for this build (-AcceptEula
  wix7 in the project).
.PARAMETER IncludeMoe
  Include moe.bin, compiled from the Ministry of Education dictionary (CC BY-ND 3.0 TW).
  Check that its license allows it before distributing such an installer.
.PARAMETER SkipBuild
  Use the existing build outputs.
#>
param(
  [switch] $IncludeMoe,
  [switch] $SkipBuild
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$out = Join-Path $repo 'build\installer'
$stage = Join-Path $out 'stage'
$env:PATH = "$env:USERPROFILE\.local\bin;$env:PATH"

function Invoke-Checked([string] $what, [scriptblock] $block) {
  & $block
  if ($LASTEXITCODE -ne 0) { throw "$what failed (exit $LASTEXITCODE)" }
}

# Version from CMakeLists.txt: project(LLaIME VERSION x.y.z ...)
$version = (Select-String -Path (Join-Path $repo 'CMakeLists.txt') -Pattern 'project\(LLaIME VERSION ([0-9.]+)').Matches[0].Groups[1].Value
Write-Host "LLaIME $version"

if (-not $SkipBuild) {
  foreach ($arch in 'x64', 'x86') {
    Invoke-Checked "CMake build ($arch)" { cmake --build (Join-Path $repo "build\$arch") --config Release }
  }
  # The about page lists the Ministry of Education dictionary only in builds that include it,
  # and the updater compares the version (from CMakeLists.txt) with the latest release.
  Invoke-Checked 'Settings app build' {
    dotnet build (Join-Path $repo 'src\Settings\App') -c Release --nologo -v quiet "-p:IncludeMoe=$($IncludeMoe.IsPresent.ToString().ToLower())" "-p:Version=$version"
  }
}

if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Force $stage | Out-Null
function Stage([string] $source, [string] $dir) {
  if (-not (Test-Path $source)) { throw "Missing $source" }
  $dest = Join-Path $stage $dir
  New-Item -ItemType Directory -Force $dest | Out-Null
  Copy-Item $source $dest -Force
}

# TIPs
Stage (Join-Path $repo 'build\x64\src\Tip\Release\LLaIME.Tip.dll') 'x64'
Stage (Join-Path $repo 'build\x86\src\Tip\Release\LLaIME.Tip.dll') 'x86'

# Engine: the executable, the backends llama.cpp loads at run time, and everything they import
# from the build folder.
$engineBuild = Join-Path $repo 'build\x64\src\Engine\Release'
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath
$dumpbin = Get-ChildItem "$vs\VC\Tools\MSVC" -Recurse -Filter dumpbin.exe |
  Where-Object { $_.FullName -match 'Hostx64\\x64' } | Select-Object -First 1
$queue = [System.Collections.Generic.Queue[string]]::new()
$wanted = @{}
# Backends llama.cpp loads at run time; ggml-vulkan.dll only if llama.cpp was built with -Vulkan
# (it needs only the driver's vulkan-1.dll, a system DLL).
foreach ($name in @('LLaIME.Engine.exe', 'onnxruntime_providers_shared.dll', 'onnxruntime_providers_cuda.dll',
                   'ggml-cuda.dll', 'ggml-vulkan.dll') +
                  (Get-ChildItem $engineBuild -Filter 'ggml-cpu-*.dll').Name) {
  $queue.Enqueue($name)
}
while ($queue.Count) {
  $name = $queue.Dequeue()
  if ($wanted.ContainsKey($name.ToLower())) { continue }
  $path = Join-Path $engineBuild $name
  if (-not (Test-Path $path)) { continue }  # a system DLL
  $wanted[$name.ToLower()] = $path
  & $dumpbin.FullName /nologo /dependents $path | Select-String '^\s+(\S+\.dll)$' |
    ForEach-Object { $queue.Enqueue($_.Matches[0].Groups[1].Value) }
}
foreach ($path in $wanted.Values) { Stage $path 'engine' }
# The VC++ runtime and OpenMP, deployed app-locally next to the engine.
$redist = Get-ChildItem "$vs\VC\Redist\MSVC" -Directory | Where-Object { $_.Name -match '^\d' } |
  Sort-Object { [version]$_.Name } | Select-Object -Last 1
foreach ($dir in Get-ChildItem (Join-Path $redist.FullName 'x64') -Directory |
                 Where-Object { $_.Name -match '\.(CRT|OpenMP)$' }) {
  foreach ($dll in Get-ChildItem $dir.FullName -Filter *.dll) { Stage $dll.FullName 'engine' }
}
Write-Host "Engine: $($wanted.Count) files + VC++ runtime ($($redist.Name))"

# Settings app (self-contained build output) and helper
$appOut = Get-ChildItem (Join-Path $repo 'src\Settings\App\bin\Release') -Recurse -Filter 'LLaIME.Settings.exe' |
  Sort-Object LastWriteTime | Select-Object -Last 1
if (-not $appOut) { throw 'Settings app not built' }
Copy-Item $appOut.DirectoryName (Join-Path $stage 'settings') -Recurse
Get-ChildItem (Join-Path $stage 'settings') -Filter '*.pdb' -Recurse | Remove-Item
Stage (Join-Path $repo 'build\x64\src\Setup\Release\LLaIME.InputSetup.exe') 'setup'

# Lexicon layers and the model catalog
$layers = @('mcbopomofo', 'chewing') + $(if ($IncludeMoe) { @('moe') } else { @() })
foreach ($layer in $layers) {
  foreach ($ext in 'bin', 'ini') { Stage (Join-Path $repo "data\build\lexicons\$layer.$ext") 'data\lexicons' }
}
Write-Host "Lexicon layers: $($layers -join ', ')"
Stage (Join-Path $repo 'installer\models.json') 'models'
Rename-Item (Join-Path $stage 'models\models.json') 'catalog.json'

# Licenses
$licenses = 'licenses'
# The notices name the Ministry of Education dictionary only in builds that include it.
$notices = Get-Content (Join-Path $repo 'installer\THIRD-PARTY-NOTICES.txt') -Raw -Encoding UTF8
$notices = if ($IncludeMoe) {
  $notices -replace '（只在以 -IncludeMoe 建置的版本中）', ''
} else {
  $notices -replace '(?m)^- 教育部《重編國語辭典修訂本》.*\r?\n.*dict\.revised\.moe\.edu\.tw.*\r?\n', ''
}
New-Item -ItemType Directory -Force (Join-Path $stage $licenses) | Out-Null
[IO.File]::WriteAllText((Join-Path $stage "$licenses\THIRD-PARTY-NOTICES.txt"), $notices, [Text.UTF8Encoding]::new($true))
$licenseFiles = @{
  'McBopomofo.txt' = 'data\raw\McBopomofo\LICENSE.txt'
  'LGPL-2.1.txt' = 'installer\licenses\LGPL-2.1.txt'
  'llama.cpp.txt' = 'third_party\llama.cpp\LICENSE'
}
foreach ($entry in $licenseFiles.GetEnumerator()) {
  $source = Join-Path $repo $entry.Value
  if (-not (Test-Path $source)) { throw "Missing license file $source" }
  Copy-Item $source (Join-Path $stage "$licenses\$($entry.Key)")
}
$nuget = Join-Path $env:USERPROFILE '.nuget\packages'
$licenseFiles = @{
  'WindowsAppSDK.txt' = 'microsoft.windowsappsdk\2.5.1\license.txt'
  'WindowsCommunityToolkit.txt' = 'communitytoolkit.winui.controls.settingscontrols\8.2.251219\License.md'
}
foreach ($entry in $licenseFiles.GetEnumerator()) {
  Copy-Item (Join-Path $nuget $entry.Value) (Join-Path $stage "$licenses\$($entry.Key)")
}
$ort = Get-ChildItem (Join-Path $repo 'build\onnxruntime') -Recurse -Filter 'LICENSE*' | Select-Object -First 1
if ($ort) { Copy-Item $ort.FullName (Join-Path $stage "$licenses\onnxruntime.txt") }

# The license page of the installer shows the notices (RTF, Unicode escapes).
$rtf = New-Object System.Text.StringBuilder
[void]$rtf.Append('{\rtf1\ansi\ansicpg950\deff0{\fonttbl{\f0\fnil Microsoft JhengHei UI;}}\fs18 ')
foreach ($ch in $notices.ToCharArray()) {
  $code = [int]$ch
  if ($ch -eq "`r") { continue }
  elseif ($ch -eq "`n") { [void]$rtf.Append('\par ') }
  elseif ($ch -eq '\' -or $ch -eq '{' -or $ch -eq '}') { [void]$rtf.Append('\' + $ch) }
  elseif ($code -lt 128) { [void]$rtf.Append($ch) }
  else { [void]$rtf.Append('\u' + ($(if ($code -gt 32767) { $code - 65536 } else { $code })) + '?') }
}
[void]$rtf.Append('}')
[IO.File]::WriteAllText((Join-Path $out 'License.rtf'), $rtf.ToString(), [Text.Encoding]::ASCII)

$size = (Get-ChildItem $stage -Recurse -File | Measure-Object Length -Sum).Sum
Write-Host ("Staged {0:N0} MB in {1}" -f ($size / 1MB), $stage)

# MSI
$name = "LLaIME-$version-x64" + $(if ($IncludeMoe) { '-moe' } else { '' })
$msi = "$name.msi"
Invoke-Checked 'WiX build' {
  # Rebuild: the staged files are not inputs the project tracks.
  dotnet build (Join-Path $repo 'installer\LLaIME.Installer.wixproj') -c Release --nologo -v minimal -t:Rebuild `
    "-p:ProductVersion=$version" "-p:StageDir=$stage\" "-p:LicenseRtf=$out\License.rtf" `
    "-p:OutputName=$name" "-p:OutputPath=$out\wix\"
}
# Each build cleans its output folder; keep the MSI next to the other variant.
Copy-Item (Join-Path $out "wix\$msi") $out -Force
$built = Join-Path $out $msi
Write-Host ("Built {0} ({1:N0} MB)" -f $built, ((Get-Item $built).Length / 1MB))
