# Dot-sourced by setup.ps1 and fetch.ps1.
#
# A sync client or antivirus program can hold a file for a moment just after it
# was written, and Windows then refuses to rename or delete the folder around it
# ("Access to the path ... is denied"). Invoke-GbRetry runs a file operation
# again with a short back-off, for about nine seconds, and then lets the last
# error through unchanged, so the step's own message still reaches the player.
function Invoke-GbRetry([scriptblock]$GbAction, [string]$GbWhat = '') {
    $ErrorActionPreference = 'Stop'
    $gbDelays = 100, 200, 400, 800, 1500, 2000, 2000, 2000
    for ($gbTry = 0; ; $gbTry++) {
        try { & $GbAction; return }
        catch {
            if ($gbTry -ge $gbDelays.Count) { throw }
            if ($gbTry -eq 0 -and $GbWhat) { Write-Host "  $GbWhat is in use by another program; trying again for a few seconds" }
            Start-Sleep -Milliseconds $gbDelays[$gbTry]
        }
    }
}
