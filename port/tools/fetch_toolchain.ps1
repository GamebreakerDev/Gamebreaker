# Fetch the compiler and build tools the Clang build needs, into ..\..\toolchain\
# (not in git). Nothing is installed and PATH is not changed; deleting the
# folder removes everything.
#
#   powershell -ExecutionPolicy Bypass -File port\tools\fetch_toolchain.ps1
#
# The downloads, their addresses and SHA-256 are in setup\tools.lock, the same
# list the player's setup uses (setup\fetch.ps1 does the work):
#
#   llvm-mingw   Clang, the LLD linker and the MinGW-w64 Windows headers and
#                libraries, 191 MB, about 720 MB unpacked
#   ninja        runs one compiler per processor core, 0.3 MB
#   cmake        a portable CMake, 54 MB. regen.ps1 uses it when it is here,
#                otherwise the one on PATH or inside Visual Studio
#
# Python is not fetched for the developer: regen.ps1 runs the lifter with the
# installed one (py -3, with capstone).

param([string[]]$Only = @('llvm-mingw', 'ninja', 'cmake'))

$ErrorActionPreference = 'Stop'
$repo = Split-Path (Split-Path $PSScriptRoot)
$root = Join-Path $repo 'toolchain'
& (Join-Path $repo 'setup\fetch.ps1') -Root $root -Only $Only
Write-Host "toolchain ready in $root"
