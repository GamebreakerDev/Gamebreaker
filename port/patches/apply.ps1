# Apply this project's patches to the xboxrecomp clone next to port\.
#
#   powershell -ExecutionPolicy Bypass -File port\patches\apply.ps1
#
# Safe to run again: a patch that is already applied is left alone. regen.ps1
# runs this before building. The patches are written against the toolkit
# commit named below; on another commit they may not apply, and the script
# says so instead of guessing.
#
# To add or change a patch: edit the files inside ..\xboxrecomp, then from the
# repository root run
#   git -C xboxrecomp diff --output=..\port\patches\NNNN-name.patch
# (all current toolkit changes in one file), and describe it in README.md.

$ErrorActionPreference = 'Stop'
$PinnedCommit = '343b6d8'      # xboxrecomp v0.17.0

# Run git and return only its exit code. Windows PowerShell 5.1 turns anything
# a program writes to stderr into an error record, and under 'Stop' that ends
# the script. "git apply --check --reverse" on an unpatched tree prints
# "error: patch failed" to stderr by design, so the checks below must not run
# under 'Stop' (this killed every fresh clone before the fix). Judge git by its
# exit code only.
function Invoke-GitQuiet([string[]]$GitArgs) {
    $old = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try { & git @GitArgs 2>&1 | Out-Null } finally { $ErrorActionPreference = $old }
    return $LASTEXITCODE
}

$toolkit = Join-Path (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path 'xboxrecomp'
if (-not (Test-Path (Join-Path $toolkit '.git'))) { throw "xboxrecomp clone not found at $toolkit" }
$head = (& git -C $toolkit rev-parse --short=7 HEAD).Trim()
if ($head -ne $PinnedCommit) {
    Write-Host "note: xboxrecomp is at $head, the patches were written against $PinnedCommit"
}

foreach ($patch in Get-ChildItem $PSScriptRoot -Filter '*.patch' | Sort-Object Name) {
    if ((Invoke-GitQuiet @('-C', $toolkit, 'apply', '--check', '--reverse', $patch.FullName)) -eq 0) {
        Write-Host "already applied: $($patch.Name)"; continue
    }
    if ((Invoke-GitQuiet @('-C', $toolkit, 'apply', '--check', $patch.FullName)) -ne 0) {
        throw "$($patch.Name) does not apply to xboxrecomp at $head. Check out $PinnedCommit, or update the patch."
    }
    if ((Invoke-GitQuiet @('-C', $toolkit, 'apply', $patch.FullName)) -ne 0) {
        throw "git apply failed for $($patch.Name)"
    }
    Write-Host "applied: $($patch.Name)"
}
