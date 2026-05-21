# Regenerate scan_tree fixture files with deterministic content.
$ErrorActionPreference = "Stop"
$root = Join-Path $PSScriptRoot "scan_tree"
New-Item -ItemType Directory -Force -Path (Join-Path $root "sub") | Out-Null
Set-Content -Path (Join-Path $root "file_a.txt") -Value "hello world" -NoNewline -Encoding utf8
Set-Content -Path (Join-Path $root "file_b.txt") -Value "second file" -NoNewline -Encoding utf8
Set-Content -Path (Join-Path $root "sub\nested.txt") -Value "nested here" -NoNewline -Encoding utf8
Write-Host "scan_tree fixture updated under $root"
