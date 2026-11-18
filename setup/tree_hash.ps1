# A fingerprint of a folder of files that does not depend on how they were
# packed: GitHub's archive of a commit, a git archive made with Windows line
# endings, or a clone. Dot-source this file. Used by fetch.ps1 (the toolkit
# from its upstream repository) and setup.ps1 (a toolkit already present).
#
# The fingerprint, exactly:
#   - every file under the folder, except anything inside .git, __pycache__ or
#     tools\<name>\output (the toolkit's own analysis output in a working clone);
#   - each file's relative path, with forward slashes;
#   - each file's SHA-256 over its bytes, where a file with no NUL byte counts as
#     text and every CR that comes right before an LF is removed first (so CRLF
#     and LF give the same hash); any other file is hashed as it is;
#   - one line per file, "<sha256 in lower case>  <path>", sorted by path
#     (ordinal), joined with LF, and the SHA-256 of that text in UTF-8.

function Get-GbTreeHash([string]$Root) {
    $Root = (Resolve-Path -LiteralPath $Root).Path.TrimEnd('\')
    $skip = '(^|/)(\.git|__pycache__)(/|$)|^tools/[^/]+/output(/|$)'
    $sha = [Security.Cryptography.SHA256]::Create()
    $latin1 = [Text.Encoding]::GetEncoding(28591)     # one byte = one character, both ways: an exact byte rewrite
    $lines = New-Object Collections.Generic.List[string]
    foreach ($f in Get-ChildItem -LiteralPath $Root -Recurse -File -Force) {
        $rel = $f.FullName.Substring($Root.Length + 1).Replace('\', '/')
        if ($rel -match $skip) { continue }
        $b = [IO.File]::ReadAllBytes($f.FullName)
        if ([Array]::IndexOf($b, [byte]0) -lt 0) {
            $b = $latin1.GetBytes($latin1.GetString($b).Replace("`r`n", "`n"))    # text: CRLF -> LF
        }
        $h = -join ($sha.ComputeHash($b) | ForEach-Object { $_.ToString('x2') })
        $lines.Add("$h  $rel")
    }
    $arr = $lines.ToArray(); [Array]::Sort($arr, [StringComparer]::Ordinal)
    $text = [Text.Encoding]::UTF8.GetBytes(($arr -join "`n"))
    [pscustomobject]@{ Hash = (-join ($sha.ComputeHash($text) | ForEach-Object { $_.ToString('x2') })); Files = $arr.Count }
}
