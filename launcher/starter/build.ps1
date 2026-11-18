# Builds Gamebreaker.exe, the small starter program that opens the launcher
# (see starter.c), with the pinned llvm-mingw in the developer's toolchain\
# folder (port\tools\fetch_toolchain.ps1 fetches it; nothing else is needed).
#
#   powershell -ExecutionPolicy Bypass -File launcher\starter\build.ps1 [-Version 0.1.0] [-Out <file>]
#
# Default output: Gamebreaker.exe at the top of the repository (ignored by git).
# The build is reproducible: no timestamp is written, so the same source and
# compiler give the same bytes. Prints the output's size and SHA-256.
param(
    [string]$Version = 'dev',
    [string]$Out = '',
    [string]$Toolchain = '',     # a folder holding llvm-mingw-<ver>-ucrt-x86_64\bin (default: <repo>\toolchain\llvm-mingw)
    [string]$Work = ''           # for the intermediate files (default: <repo>\out\starter)
)

$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$repo = Split-Path (Split-Path $here)
if (-not $Out) { $Out = Join-Path $repo 'Gamebreaker.exe' }
$Out = [IO.Path]::GetFullPath($Out)

# The compiler named in setup\tools.lock, so the starter is built with the same
# llvm-mingw the game is.
$lock = Get-Content (Join-Path $repo 'setup\tools.lock') -Raw | ConvertFrom-Json
$lm = $lock.tools | Where-Object { $_.name -eq 'llvm-mingw' }
if (-not $Toolchain) { $Toolchain = Join-Path $repo ('toolchain\' + $lm.dir) }
$clang = Join-Path $Toolchain $lm.check
if (-not (Test-Path $clang)) { throw "llvm-mingw $($lm.version) not found at $clang. Run port\tools\fetch_toolchain.ps1 first." }
$bin = Split-Path $clang
$windres = Join-Path $bin 'x86_64-w64-mingw32-windres.exe'

# Version: "0.1.0-dev" -> 0,1,0,0 for the numeric fields; the text as given.
$nums = @(([regex]::Matches($Version, '\d+') | Select-Object -First 4 | ForEach-Object { [int]$_.Value }))
while ($nums.Count -lt 4) { $nums += 0 }
$tmp = $(if ($Work) { $Work } else { Join-Path $repo 'out\starter' })
New-Item -ItemType Directory -Force $tmp | Out-Null
$hdr = "#define GB_VER_NUM $($nums -join ',')`n#define GB_VER_TEXT `"$($Version -replace '[^A-Za-z0-9.+-]', '')`"`n"
[IO.File]::WriteAllText((Join-Path $tmp 'gb_version.h'), $hdr, (New-Object Text.UTF8Encoding($false)))

$res = Join-Path $tmp 'starter.res.o'
Push-Location $here
try {
    & $windres -I $tmp -O coff -o $res starter.rc
    if ($LASTEXITCODE) { throw "windres failed ($LASTEXITCODE)" }
    New-Item -ItemType Directory -Force (Split-Path $Out) | Out-Null
    $defs = @()
    & $clang -std=c11 -O2 -Wall -Wextra -municode -mwindows -s @defs -o $Out starter.c $res '-Wl,--no-insert-timestamp' -luser32
    if ($LASTEXITCODE) { throw "clang failed ($LASTEXITCODE)" }
} finally { Pop-Location }

$h = (Get-FileHash $Out -Algorithm SHA256).Hash
'{0}  {1:N0} bytes  SHA-256 {2}  (version {3}, llvm-mingw {4})' -f $Out, (Get-Item $Out).Length, $h, $Version, $lm.version
