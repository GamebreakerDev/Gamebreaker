# Fetches Microsoft's WebView2 SDK package from nuget.org and keeps the three
# DLLs the host window needs, in .\lib (not tracked in git).
#
# The package is pinned by version and SHA-256. The DLLs must also carry a
# valid Microsoft signature, or the script stops. Nothing is installed: delete
# .\lib to remove it all.
param([switch]$Force)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

$Version = '1.0.4258.31'
$Sha256  = '56F7F4B8BF9AEE4B8EFEFBBDD4F67D5F74EBD1B100ED0806DA71BF76AF481AA9'
$Url     = "https://api.nuget.org/v3-flatcontainer/microsoft.web.webview2/$Version/microsoft.web.webview2.$Version.nupkg"

$Lib   = Join-Path $PSScriptRoot 'lib'
$Wants = @{
    'lib/net462/Microsoft.Web.WebView2.Core.dll'     = 'Microsoft.Web.WebView2.Core.dll'
    'lib/net462/Microsoft.Web.WebView2.WinForms.dll' = 'Microsoft.Web.WebView2.WinForms.dll'
    'runtimes/win-x64/native/WebView2Loader.dll'     = 'WebView2Loader.dll'
    'LICENSE.txt'                                    = 'LICENSE.txt'
    'NOTICE.txt'                                     = 'NOTICE.txt'
}
$Stamp = Join-Path $Lib 'version.txt'

$have = (Test-Path $Stamp) -and ((Get-Content $Stamp -Raw).Trim() -eq $Version)
foreach ($name in $Wants.Values) { if (-not (Test-Path (Join-Path $Lib $name))) { $have = $false } }
if ($have -and -not $Force) { return }

New-Item -ItemType Directory -Force $Lib | Out-Null
$pkg = Join-Path $Lib 'package.zip'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
Write-Host "Fetching WebView2 SDK $Version from nuget.org"
Invoke-WebRequest -Uri $Url -OutFile $pkg -UseBasicParsing

$got = (Get-FileHash $pkg -Algorithm SHA256).Hash
if ($got -ne $Sha256) {
    Remove-Item $pkg -Force
    throw "WebView2 SDK package hash mismatch. Expected $Sha256, got $got."
}

Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip = [IO.Compression.ZipFile]::OpenRead($pkg)
try {
    foreach ($entry in $zip.Entries) {
        if ($Wants.ContainsKey($entry.FullName)) {
            [IO.Compression.ZipFileExtensions]::ExtractToFile($entry, (Join-Path $Lib $Wants[$entry.FullName]), $true)
        }
    }
} finally { $zip.Dispose() }
Remove-Item $pkg -Force

foreach ($name in $Wants.Values | Where-Object { $_ -like '*.dll' }) {
    $path = Join-Path $Lib $name
    if (-not (Test-Path $path)) { throw "The package did not contain $name." }
    $sig = Get-AuthenticodeSignature $path
    if ($sig.Status -ne 'Valid' -or $sig.SignerCertificate.Subject -notmatch 'O=Microsoft Corporation') {
        throw "$name does not carry a valid Microsoft signature ($($sig.Status))."
    }
}
Set-Content -Path $Stamp -Value $Version -Encoding ascii
Write-Host "WebView2 SDK $Version ready in $Lib"
