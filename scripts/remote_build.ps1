# ============================================================================
# ntplite - scripts/remote_build.ps1
# ----------------------------------------------------------------------------
# Synchronise the working tree to the Linux development host and run the full
# configure / build / test cycle there.
#
# The project targets Windows and Linux equally, so a green CI run is not
# enough - this script is the "second opinion" that catches things a
# GNU/Linux toolchain sees differently from MSVC.
#
# Examples
# --------
#   pwsh scripts/remote_build.ps1                      # Debug build + ctest
#   pwsh scripts/remote_build.ps1 -BuildType Release
#   pwsh scripts/remote_build.ps1 -Sanitize            # ASan + UBSan
#   pwsh scripts/remote_build.ps1 -SkipSync            # reuse what is there
#   pwsh scripts/remote_build.ps1 -RemoteDir '~/tmp/ntplite'
# ============================================================================
[CmdletBinding()]
param(
  [string] $Target = 'leid@10.0.228.100',
  [string] $RemoteDir = '~/projects/ntplite',

  [ValidateSet('Debug', 'Release', 'RelWithDebInfo', 'MinSizeRel')]
  [string] $BuildType = 'Debug',

  [switch] $Sanitize,
  [switch] $SkipSync,
  [switch] $OnlineTests,
  [switch] $KeepBuildDirectory
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$RepositoryRoot = Split-Path -Parent $PSScriptRoot

function Write-Step {
  param([string] $Message)
  Write-Host ''
  Write-Host "==> $Message" -ForegroundColor Cyan
}

function Invoke-Remote {
  param([string] $Command)
  & ssh -o BatchMode=yes -o ConnectTimeout=15 $Target $Command
  if ($LASTEXITCODE -ne 0) {
    throw "remote command failed (exit $LASTEXITCODE): $Command"
  }
}

# ---------------------------------------------------------------------------
# Guard rails: refuse to delete anything outside a normal project directory.
# ---------------------------------------------------------------------------
if ([string]::IsNullOrWhiteSpace($RemoteDir)) { throw 'RemoteDir must not be empty' }
$normalised = $RemoteDir.TrimEnd('/')
if ($normalised -in @('~', '$HOME', '/', '/home', '/root', '', '.')) {
  throw "refusing to use '$RemoteDir' as the remote directory: too dangerous to clean"
}

Write-Host "ntplite remote build" -ForegroundColor Green
Write-Host "  target      : $Target"
Write-Host "  remote dir  : $RemoteDir"
Write-Host "  build type  : $BuildType"
Write-Host "  sanitizers  : $([bool]$Sanitize)"

# ---------------------------------------------------------------------------
# 1. Connectivity and toolchain check
# ---------------------------------------------------------------------------
Write-Step 'Checking the remote toolchain'
Invoke-Remote 'set -e; echo "host: $(uname -srm)"; echo -n "gcc: "; gcc --version | head -n1; echo -n "clang: "; (clang --version 2>/dev/null || echo "not installed") | head -n1; echo -n "cmake: "; cmake --version | head -n1; echo -n "ninja: "; (ninja --version 2>/dev/null || echo "not installed"); echo -n "python3: "; python3 --version'

# ---------------------------------------------------------------------------
# 2. Synchronise the sources
# ---------------------------------------------------------------------------
if (-not $SkipSync) {
  Write-Step 'Synchronising sources (tar over scp)'

  $archive = Join-Path ([System.IO.Path]::GetTempPath()) 'ntplite-sync.tar.gz'
  if (Test-Path $archive) { Remove-Item -Force $archive }

  & tar -czf $archive -C $RepositoryRoot `
    --exclude=./build --exclude=./.git --exclude=./_install --exclude=./dist `
    --exclude=./__pycache__ .
  if ($LASTEXITCODE -ne 0) { throw 'tar failed' }

  $sizeKb = [math]::Round((Get-Item $archive).Length / 1KB, 1)
  Write-Host "  archive: $archive ($sizeKb KiB)"

  & scp -q $archive "${Target}:/tmp/ntplite-sync.tar.gz"
  if ($LASTEXITCODE -ne 0) { throw 'scp failed' }

  Invoke-Remote "set -e; mkdir -p $RemoteDir; find $RemoteDir -mindepth 1 -maxdepth 1 -exec rm -rf {} +; tar -xzf /tmp/ntplite-sync.tar.gz -C $RemoteDir; rm -f /tmp/ntplite-sync.tar.gz; ls -1 $RemoteDir"

  Remove-Item -Force $archive
}

# ---------------------------------------------------------------------------
# 3. Configure, build, test
# ---------------------------------------------------------------------------
$sanitizeFlag = if ($Sanitize) { 'ON' } else { 'OFF' }
$onlineFlag = if ($OnlineTests) { 'ON' } else { 'OFF' }

$clean = if ($KeepBuildDirectory) { '' } else { 'rm -rf build-remote;' }

Write-Step 'Configure / build / test'

$remoteScript = @"
set -e
cd $RemoteDir
$clean
cmake -S . -B build-remote -G Ninja \
  -DCMAKE_BUILD_TYPE=$BuildType \
  -DNTP_LITE_WERROR=ON \
  -DNTP_LITE_SANITIZE=$sanitizeFlag \
  -DNTP_LITE_ONLINE_TESTS=$onlineFlag
cmake --build build-remote --parallel
ctest --test-dir build-remote --output-on-failure
"@

Invoke-Remote $remoteScript

Write-Host ''
Write-Host 'remote build: PASSED' -ForegroundColor Green
