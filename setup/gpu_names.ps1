# Graphics adapters by name: which are built into the processor, and which one the
# game will draw on. Dot-sourced by setup.ps1 (the "This PC" line) and by the
# launcher's host.ps1 (lighter defaults on integrated graphics only), so both
# read an adapter list the same way.
#
# Windows does not say which adapters are built into the processor, and an
# integrated chip's AdapterRAM is its share of main memory (so on a laptop with
# two chips the integrated one often reports more), so the name decides. A name
# not recognised counts as a dedicated card.

# Names that are not a graphics chip the game can draw on.
$script:GbGpuSkip = 'Basic|Virtual|Parsec|Remote|Microsoft|Mirror'

function Test-IntegratedGpu([string]$name) {
    if ($name -match 'NVIDIA|GeForce|Quadro|RTX|GTX') { return $false }
    if ($name -match 'Intel') { return -not ($name -match '\bArc(\(TM\))?\s+(Pro\s+)?[AB]\d') }    # Arc A and B series are cards; Arc 130T, 140T, 140V are built in
    if ($name -match 'AMD|Radeon|\bATI\b') {
        if ($name -match 'Radeon(\(TM\))?\s+(RX|Pro|VII|HD\s+\d|R\d\s)') { return $false }
        return $name -match 'Radeon(\(TM\))?\s+(Graphics|Vega|\d{3,4}[MS])|Custom GPU|Radeon\(TM\)\s*$'
    }
    return $name -match 'Adreno|Mali'
}

# The adapters Windows reports as working, without virtual and remote ones.
# Each item needs Name and Status (Win32_VideoController's fields). A chip
# stopped by Windows (Status Error, for example code 43) is left out: the game
# cannot draw on it.
function Get-UsableGpu([object[]]$adapters) {
    @($adapters | Where-Object { $_ -and $_.Name -and [string]$_.Status -eq 'OK' -and [string]$_.Name -notmatch $script:GbGpuSkip })
}

# The chip the game draws on: a working dedicated card when there is one (the
# game asks Windows for the high-performance chip), otherwise the integrated
# one. Several cards: the one with the most memory. $null when none works.
function Select-GameGpu([object[]]$adapters) {
    $ok = @(Get-UsableGpu $adapters)
    $cards = @($ok | Where-Object { -not (Test-IntegratedGpu ([string]$_.Name)) } | Sort-Object { [long]$_.AdapterRAM } -Descending)
    if ($cards.Count) { return [string]$cards[0].Name }
    if ($ok.Count) { return [string]$ok[0].Name }
    $null
}
