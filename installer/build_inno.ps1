# DiskAtlas Inno Setup Build Script
# Requires Inno Setup to be installed

param(
    [string]$Version = "1.0.0",
    [string]$SourceDir = "..",
    [string]$OutputDir = "..\dist"
)

# Check if Inno Setup is installed
$innoPath = $null
$commonPaths = @(
    "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
    "${env:ProgramFiles}\Inno Setup 6\ISCC.exe",
    "${env:ProgramFiles(x86)}\Inno Setup 5\ISCC.exe",
    "${env:ProgramFiles}\Inno Setup 5\ISCC.exe"
)

foreach ($path in $commonPaths) {
    if (Test-Path $path) {
        $innoPath = $path
        break
    }
}

if (-not $innoPath) {
    Write-Host "Error: Inno Setup not found. Please install Inno Setup first." -ForegroundColor Red
    Write-Host "Download from: https://jrsoftware.org/isinfo.php" -ForegroundColor Yellow
    exit 1
}

Write-Host "Found Inno Setup at: $innoPath" -ForegroundColor Green

# Resolve paths
$SourceDir = Resolve-Path $SourceDir
$OutputDir = Resolve-Path $OutputDir -ErrorAction SilentlyContinue
if (-not $OutputDir) {
    New-Item -ItemType Directory -Path $OutputDir -Force | Out-Null
    $OutputDir = Resolve-Path $OutputDir
}

Write-Host "Building DiskAtlas Installer with Inno Setup..." -ForegroundColor Green
Write-Host "Version: $Version" -ForegroundColor Cyan
Write-Host "Source Directory: $SourceDir" -ForegroundColor Cyan
Write-Host "Output Directory: $OutputDir" -ForegroundColor Cyan

# Sign the executable before packaging (optional if sign script exists)
$ExeToSign = "$SourceDir\dist\DiskAtlas\diskatlas.exe"
$CertPath = "$SourceDir\cert\Mechanika Design.pfx"
$SignScript = "$SourceDir\scripts\sign_exe.ps1"
if ((Test-Path $SignScript) -and (Test-Path $ExeToSign) -and (Test-Path $CertPath)) {
    Write-Host "Signing executable before packaging..." -ForegroundColor Yellow
    & powershell -ExecutionPolicy Bypass -File $SignScript -ExePath $ExeToSign -CertPath $CertPath
    if ($LASTEXITCODE -ne 0) {
        Write-Host "Error: Failed to sign executable. Build aborted." -ForegroundColor Red
        exit 1
    }
    Write-Host "Executable signed successfully." -ForegroundColor Green
} elseif (Test-Path $ExeToSign) {
    Write-Host "Skipping code signing (sign_exe.ps1 or certificate not found)." -ForegroundColor Yellow
} else {
    Write-Host "Error: Executable not found at $ExeToSign" -ForegroundColor Red
    exit 1
}

# Check if required files exist
$requiredFiles = @(
    "$SourceDir\dist\DiskAtlas\diskatlas.exe",
    "$SourceDir\dist\DiskAtlas\diskatlas_core.dll",
    "$SourceDir\dist\DiskAtlas\readme.txt"
)

$portablePackageDir = "$SourceDir\dist\DiskAtlas"
if (-not (Test-Path $portablePackageDir)) {
    Write-Host "Error: Portable package not found at $portablePackageDir" -ForegroundColor Red
    Write-Host "Please run the portable package script first to create the required files." -ForegroundColor Yellow
    Write-Host "Run: .\Create-PortablePackage.ps1" -ForegroundColor Yellow
    exit 1
}

foreach ($file in $requiredFiles) {
    if (-not (Test-Path $file)) {
        Write-Host "Error: Required file not found: $file" -ForegroundColor Red
        Write-Host "Please ensure the DiskAtlas application is built first." -ForegroundColor Yellow
        exit 1
    }
}

try {
    Write-Host "Building installer with Inno Setup..." -ForegroundColor Yellow

    $scriptContent = Get-Content "DiskAtlas.iss" -Raw
    $scriptContent = $scriptContent -replace '#define MyAppVersion ".*"', "#define MyAppVersion `"$Version`""
    $scriptContent | Set-Content "DiskAtlas_temp.iss"

    $result = & $innoPath "DiskAtlas_temp.iss"

    Remove-Item "DiskAtlas_temp.iss" -ErrorAction SilentlyContinue

    if ($LASTEXITCODE -eq 0) {
        Write-Host "Installer created successfully!" -ForegroundColor Green

        $installerFile = Get-ChildItem "$OutputDir\diskatlas-$Version-win64-setup.exe" -ErrorAction SilentlyContinue
        if ($installerFile) {
            Write-Host "Location: $($installerFile.FullName)" -ForegroundColor Cyan
            $sizeInMB = [math]::Round($installerFile.Length / 1MB, 2)
            Write-Host "Size: $sizeInMB MB" -ForegroundColor Cyan
        }
    } else {
        throw "Inno Setup build failed with exit code $LASTEXITCODE"
    }

} catch {
    Write-Host "Error: $_" -ForegroundColor Red
    exit 1
}

Write-Host "Build completed successfully!" -ForegroundColor Green
