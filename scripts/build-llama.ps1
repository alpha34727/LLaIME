<#
.SYNOPSIS
  Builds llama.cpp (third_party/llama.cpp) with dynamically loaded ggml backends.
.DESCRIPTION
  Uses the Visual Studio developer environment with Ninja (the CUDA MSBuild integration does not
  cover every Visual Studio release). Backends are built as separate DLLs (GGML_BACKEND_DL) so
  the engine can offer CPU / CUDA / Vulkan / ... and load whichever the user picks.
  Output is installed to build\llama\<Configuration>\install.
.PARAMETER Configuration
  Release (default) or Debug.
.PARAMETER CudaArchitectures
  CUDA SM list. Default "120-real" (RTX 50 series) for development; use e.g.
  "75-real;86-real;89-real;120-real;120-virtual" for distribution builds.
.PARAMETER Vulkan
  Also build the Vulkan backend (requires the Vulkan SDK, VULKAN_SDK env var).
#>
param(
  [ValidateSet('Release', 'Debug')] [string] $Configuration = 'Release',
  [string] $CudaArchitectures = '120-real',
  [switch] $NoCuda,
  [switch] $Vulkan
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$source = Join-Path $repo 'third_party\llama.cpp'
$buildDir = Join-Path $repo "build\llama\$Configuration"
$installDir = Join-Path $buildDir 'install'

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsPath) { throw 'Visual Studio with C++ tools not found' }
& (Join-Path $vsPath 'Common7\Tools\Launch-VsDevShell.ps1') -Arch amd64 -HostArch amd64 -SkipAutomaticLocation | Out-Null

# cmake/ninja from `uv tool install`.
$env:PATH = "$env:USERPROFILE\.local\bin;$env:PATH"

$cmakeArgs = @(
  '-S', $source, '-B', $buildDir, '-G', 'Ninja',
  "-DCMAKE_BUILD_TYPE=$Configuration",
  "-DCMAKE_INSTALL_PREFIX=$installDir",
  '-DBUILD_SHARED_LIBS=ON',
  '-DGGML_BACKEND_DL=ON',
  '-DGGML_CPU_ALL_VARIANTS=ON',
  '-DGGML_NATIVE=OFF',
  '-DLLAMA_CURL=OFF',
  '-DLLAMA_BUILD_TESTS=OFF',
  '-DLLAMA_BUILD_EXAMPLES=OFF',
  # The unified `llama` tool links the server implementation, so keep it enabled.
  '-DLLAMA_BUILD_SERVER=ON',
  '-DLLAMA_BUILD_TOOLS=ON'
)
if (-not $NoCuda) {
  # Use the machine-wide CUDA_PATH (the newest installed toolkit), not a stale session value.
  $cudaPath = [Environment]::GetEnvironmentVariable('CUDA_PATH', 'Machine')
  if (-not $cudaPath) { throw 'CUDA Toolkit not found (CUDA_PATH)' }
  $env:CUDA_PATH = $cudaPath
  $cmakeArgs += @(
    '-DGGML_CUDA=ON',
    "-DCUDAToolkit_ROOT=$cudaPath",
    "-DCMAKE_CUDA_COMPILER=$(Join-Path $cudaPath 'bin/nvcc.exe')",
    "-DCMAKE_CUDA_ARCHITECTURES=$CudaArchitectures"
  )
}
if ($Vulkan) {
  if (-not $env:VULKAN_SDK) { throw 'VULKAN_SDK is not set; install the Vulkan SDK first' }
  $cmakeArgs += '-DGGML_VULKAN=ON'
}

cmake @cmakeArgs
if ($LASTEXITCODE -ne 0) { throw 'cmake configure failed' }
cmake --build $buildDir --config $Configuration
if ($LASTEXITCODE -ne 0) { throw 'build failed' }
cmake --install $buildDir --config $Configuration
if ($LASTEXITCODE -ne 0) { throw 'install failed' }
Write-Host "llama.cpp installed to $installDir"
