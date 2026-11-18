# Signing a release's list of files, so an update can tell that the list comes
# from the project and not only from wherever it was downloaded. Dot-sourced by
# update.ps1 (which checks) and make_release.ps1 (which signs).
#
# ECDSA on the P-256 curve with SHA-256, from .NET Framework's own CNG classes:
# nothing to download or install. Keys and signatures are Base64 text:
#   the private key   CNG's EccPrivateBlob, 104 bytes (only on the maintainer's PC)
#   the public key    CNG's EccPublicBlob, 72 bytes (setup\release_key.txt, in every release)
#   a signature       the 64 bytes r and s (manifest.json.sig, beside manifest.json on the feed)

function Get-GbSigningKeyFile { Join-Path $env:USERPROFILE '.gamebreaker\release-signing.key' }

# The key line of a key file: the first line that is not a comment.
function Read-GbKeyText([string]$Path) {
    foreach ($l in [IO.File]::ReadAllLines($Path)) { $t = $l.Trim(); if ($t -and -not $t.StartsWith('#')) { return $t } }
    ''
}

function New-GbKeyPair {
    $p = New-Object Security.Cryptography.CngKeyCreationParameters
    $p.ExportPolicy = [Security.Cryptography.CngExportPolicies]::AllowPlaintextExport
    $k = [Security.Cryptography.CngKey]::Create([Security.Cryptography.CngAlgorithm]::ECDsaP256, [NullString]::Value, $p)   # no name: a key that lives only in memory
    try {
        @{ Private = [Convert]::ToBase64String($k.Export([Security.Cryptography.CngKeyBlobFormat]::EccPrivateBlob))
           Public  = [Convert]::ToBase64String($k.Export([Security.Cryptography.CngKeyBlobFormat]::EccPublicBlob)) }
    } finally { $k.Dispose() }
}

function Get-GbPublicKey([string]$Private) {
    $k = [Security.Cryptography.CngKey]::Import([Convert]::FromBase64String($Private.Trim()), [Security.Cryptography.CngKeyBlobFormat]::EccPrivateBlob)
    try { [Convert]::ToBase64String($k.Export([Security.Cryptography.CngKeyBlobFormat]::EccPublicBlob)) } finally { $k.Dispose() }
}

function Get-GbDigest([byte[]]$Data) {
    $h = [Security.Cryptography.SHA256]::Create()
    try { , $h.ComputeHash($Data) } finally { $h.Dispose() }
}

function New-GbSignature([byte[]]$Data, [string]$Private) {
    $k = [Security.Cryptography.CngKey]::Import([Convert]::FromBase64String($Private.Trim()), [Security.Cryptography.CngKeyBlobFormat]::EccPrivateBlob)
    try {
        $e = New-Object Security.Cryptography.ECDsaCng($k)
        try { [Convert]::ToBase64String($e.SignHash((Get-GbDigest $Data))) } finally { $e.Dispose() }
    } finally { $k.Dispose() }
}

# $true only for a well-formed signature of exactly these bytes by this key.
# Anything malformed is simply not a valid signature.
function Test-GbSignature([byte[]]$Data, [string]$Signature, [string]$Public) {
    try {
        $sig = [Convert]::FromBase64String($Signature.Trim())
        $pub = [Convert]::FromBase64String($Public.Trim())
        if ($sig.Length -ne 64 -or $pub.Length -ne 72) { return $false }
        $k = [Security.Cryptography.CngKey]::Import($pub, [Security.Cryptography.CngKeyBlobFormat]::EccPublicBlob)
        try {
            $e = New-Object Security.Cryptography.ECDsaCng($k)
            try { return [bool]$e.VerifyHash((Get-GbDigest $Data), $sig) } finally { $e.Dispose() }
        } finally { $k.Dispose() }
    } catch { return $false }
}
