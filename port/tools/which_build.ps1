# Which built program the scripts run. Prints its full path.
#
#   $exe = & port\tools\which_build.ps1
#
# The reference build is the Clang one (llvm-mingw, port\build-mingw): it is
# the build users get, so it is the one that is played and measured. The MSVC
# build (port\build\Release) is kept as a second opinion.
#
#   GB_RUN_EXE=<path>   run exactly this program (wins over everything)
#   GB_BUILD=msvc       the MSVC build (run.ps1 -Msvc and play.ps1 -Msvc set it)
#   otherwise           the Clang build; if it has not been built yet, the
#                       MSVC build, with a warning
#
# Both are environment variables so that a script which starts run.ps1 in a
# child process (lap.ps1, shots.ps1, ...) passes the choice on unchanged.
# The symbol map the tools read lies next to the program (gamebreaker.map);
# port\tools\perf\rip_report.py default_map() follows the same rule.

$port  = Split-Path $PSScriptRoot
$clang = Join-Path $port 'build-mingw\gamebreaker.exe'
$msvc  = Join-Path $port 'build\Release\gamebreaker.exe'

if ($env:GB_RUN_EXE) { return $env:GB_RUN_EXE }
if ($env:GB_BUILD -and $env:GB_BUILD -ne 'msvc' -and $env:GB_BUILD -ne 'clang') {
    throw "GB_BUILD is '$($env:GB_BUILD)'; use msvc or clang (or leave it unset)."
}
if ($env:GB_BUILD -eq 'msvc') { return $msvc }
if (-not (Test-Path $clang) -and -not $env:GB_BUILD -and (Test-Path $msvc)) {
    Write-Warning "The Clang build ($clang) does not exist yet; using the MSVC build. Run port\regen.ps1 to build it."
    return $msvc
}
return $clang
