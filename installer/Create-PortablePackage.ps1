# DiskAtlas Portable Package Creator
# Delegates to scripts/bundle_portable.sh (ldd-based DLL copy + frozen GTK runtime).

param(
    [string]$Version = "1.0.0",
    [string]$SourceDir = "..",
    [string]$OutputDir = "..\dist"
)

$MsysBash = "C:\msys64\usr\bin\bash.exe"
$SourceDir = (Resolve-Path $SourceDir).Path
$PackageDir = Join-Path (Resolve-Path $OutputDir -ErrorAction SilentlyContinue) "DiskAtlas"

Write-Host "DiskAtlas Portable Package Creator" -ForegroundColor Cyan

if (-not (Test-Path (Join-Path $SourceDir "bin\diskatlas.exe"))) {
    Write-Host "Error: build bin\diskatlas.exe first." -ForegroundColor Red
    exit 1
}

if (-not (Test-Path $MsysBash)) {
    Write-Host "Error: MSYS2 not found at $MsysBash" -ForegroundColor Red
    exit 1
}

$SignScript = Join-Path $SourceDir "scripts\sign_exe.ps1"
$CertPath = Join-Path $SourceDir "cert\Mechanika Design.pfx"
$ExePath = Join-Path $SourceDir "bin\diskatlas.exe"
if ((Test-Path $SignScript) -and (Test-Path $CertPath)) {
    Write-Host "Signing executable..." -ForegroundColor Yellow
    & powershell -ExecutionPolicy Bypass -File $SignScript -ExePath $ExePath -CertPath $CertPath
    if ($LASTEXITCODE -ne 0) { exit 1 }
}

$SourceUnix = $SourceDir -replace '\\', '/'
& $MsysBash -lc "export MSYSTEM=MINGW64; source /etc/profile; cd '$SourceUnix' && ./scripts/bundle_portable.sh"
if ($LASTEXITCODE -ne 0) {
    Write-Host "Error: bundle_portable.sh failed." -ForegroundColor Red
    exit 1
}

$ZipName = "diskatlas-$Version-win64.zip"
$ZipPath = Join-Path $OutputDir $ZipName
if (Test-Path $ZipPath) { Remove-Item $ZipPath -Force }
Compress-Archive -Path "$PackageDir\*" -DestinationPath $ZipPath -Force

Write-Host "Portable package: $PackageDir" -ForegroundColor Green
Write-Host "ZIP: $ZipPath" -ForegroundColor Green
