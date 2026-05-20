# DiskAtlas Portable Package Creator (PowerShell Version)
# Creates a portable package with all GTK dependencies bundled

param(
    [string]$Version = "1.0.0",
    [string]$SourceDir = "..",
    [string]$GTKPath = "C:\msys64\mingw64",
    [string]$OutputDir = "..\dist"
)

Write-Host "========================================" -ForegroundColor Cyan
Write-Host "DiskAtlas Portable Package Creator" -ForegroundColor Cyan
Write-Host "========================================" -ForegroundColor Cyan

$AppName = "DiskAtlas"
$SourceDir = Resolve-Path $SourceDir
$OutputDir = Resolve-Path $OutputDir -ErrorAction SilentlyContinue
if (-not $OutputDir) {
    New-Item -ItemType Directory -Path $OutputDir -Force | Out-Null
    $OutputDir = Resolve-Path $OutputDir
}

$PackageDir = Join-Path $OutputDir $AppName

Write-Host "Creating portable package for $AppName v$Version..." -ForegroundColor Green
Write-Host "Source Directory: $SourceDir" -ForegroundColor Yellow
Write-Host "Package Directory: $PackageDir" -ForegroundColor Yellow

if (Test-Path $PackageDir) {
    Remove-Item $PackageDir -Recurse -Force
}
New-Item -ItemType Directory -Path $PackageDir -Force | Out-Null

$ExePath = Join-Path $SourceDir "bin\diskatlas.exe"
$CoreDllPath = Join-Path $SourceDir "bin\diskatlas_core.dll"
if (-not (Test-Path $ExePath)) {
    Write-Host "Error: diskatlas.exe not found in $SourceDir\bin" -ForegroundColor Red
    Write-Host "Please build the application first." -ForegroundColor Yellow
    exit 1
}
if (-not (Test-Path $CoreDllPath)) {
    Write-Host "Error: diskatlas_core.dll not found in $SourceDir\bin" -ForegroundColor Red
    Write-Host "Please build the application first." -ForegroundColor Yellow
    exit 1
}

$SignScript = Join-Path $SourceDir "scripts\sign_exe.ps1"
$CertPath = Join-Path $SourceDir "cert\Mechanika Design.pfx"
if ((Test-Path $SignScript) -and (Test-Path $CertPath)) {
    Write-Host "Signing executable..." -ForegroundColor Green
    & powershell -ExecutionPolicy Bypass -File $SignScript -ExePath $ExePath -CertPath $CertPath
    if ($LASTEXITCODE -ne 0) {
        Write-Host "Error: Failed to sign executable. Build aborted." -ForegroundColor Red
        exit 1
    }
    Write-Host "Executable signed successfully." -ForegroundColor Green
} else {
    Write-Host "Skipping code signing (sign_exe.ps1 or certificate not found)." -ForegroundColor Yellow
}

Write-Host "Copying main executable and core library..." -ForegroundColor Green
Copy-Item $ExePath $PackageDir
Copy-Item $CoreDllPath $PackageDir

$ReadmePath = Join-Path $SourceDir "readme.txt"
if (Test-Path $ReadmePath) {
    Write-Host "Copying documentation..." -ForegroundColor Green
    Copy-Item $ReadmePath $PackageDir
}

$UpdateDll = Join-Path $SourceDir "bin\update.dll"
$UpdaterExe = Join-Path $SourceDir "bin\updater.exe"
if (Test-Path $UpdateDll) {
    Write-Host "Copying update.dll..." -ForegroundColor Green
    Copy-Item $UpdateDll $PackageDir
}
if (Test-Path $UpdaterExe) {
    Write-Host "Copying updater.exe..." -ForegroundColor Green
    Copy-Item $UpdaterExe $PackageDir
}

Write-Host "Analyzing dependencies..." -ForegroundColor Green

$RequiredDLLs = @(
    "libcairo-2.dll",
    "libgdk-3-0.dll",
    "libgdk_pixbuf-2.0-0.dll",
    "libgio-2.0-0.dll",
    "libgcc_s_seh-1.dll",
    "libstdc++-6.dll",
    "libglib-2.0-0.dll",
    "libgobject-2.0-0.dll",
    "libgtk-3-0.dll",
    "libfontconfig-1.dll",
    "libfreetype-6.dll",
    "libpixman-1-0.dll",
    "zlib1.dll",
    "libpng16-16.dll",
    "libintl-8.dll",
    "libgmodule-2.0-0.dll",
    "libcairo-gobject-2.dll",
    "libfribidi-0.dll",
    "libepoxy-0.dll",
    "libwinpthread-1.dll",
    "libpcre2-8-0.dll",
    "libffi-8.dll",
    "libpango-1.0-0.dll",
    "libpangocairo-1.0-0.dll",
    "libpangowin32-1.0-0.dll",
    "libatk-1.0-0.dll",
    "libharfbuzz-0.dll",
    "libexpat-1.dll",
    "libbz2-1.dll",
    "libbrotlidec.dll",
    "libiconv-2.dll",
    "libthai-0.dll",
    "libpangoft2-1.0-0.dll",
    "libbrotlicommon.dll",
    "libgraphite2.dll",
    "libdatrie-1.dll"
)

Write-Host "Found $($RequiredDLLs.Count) required dependencies" -ForegroundColor Cyan

Write-Host "Copying GTK dependencies..." -ForegroundColor Green
$CopiedCount = 0
foreach ($dll in $RequiredDLLs) {
    $dllPath = Join-Path $GTKPath "bin\$dll"
    if (Test-Path $dllPath) {
        Write-Host "  Copying $dll..." -ForegroundColor Gray
        Copy-Item $dllPath $PackageDir -ErrorAction SilentlyContinue
        $CopiedCount++
    } else {
        Write-Host "  Warning: $dll not found" -ForegroundColor Yellow
    }
}
Write-Host "Copied $CopiedCount dependencies" -ForegroundColor Cyan

Write-Host "Copying GTK themes and icons..." -ForegroundColor Green

$ShareDir = Join-Path $PackageDir "share"
New-Item -ItemType Directory -Path $ShareDir -Force | Out-Null

$ThemesSource = Join-Path $GTKPath "share\themes"
if (Test-Path $ThemesSource) {
    $ThemesDest = Join-Path $ShareDir "themes"
    Copy-Item $ThemesSource $ThemesDest -Recurse -Force -ErrorAction SilentlyContinue
}

$IconsSource = Join-Path $GTKPath "share\icons\Adwaita"
if (Test-Path $IconsSource) {
    $IconsDir = Join-Path $ShareDir "icons"
    New-Item -ItemType Directory -Path $IconsDir -Force | Out-Null
    $IconsDest = Join-Path $IconsDir "Adwaita"
    Copy-Item $IconsSource $IconsDest -Recurse -Force -ErrorAction SilentlyContinue
}

$SchemasSource = Join-Path $GTKPath "share\glib-2.0\schemas"
if (Test-Path $SchemasSource) {
    $GLibDir = Join-Path $ShareDir "glib-2.0"
    New-Item -ItemType Directory -Path $GLibDir -Force | Out-Null
    $SchemasDest = Join-Path $GLibDir "schemas"
    Copy-Item $SchemasSource $SchemasDest -Recurse -Force -ErrorAction SilentlyContinue
}

Write-Host "Copying GTK modules..." -ForegroundColor Green
$LibDir = Join-Path $PackageDir "lib"
New-Item -ItemType Directory -Path $LibDir -Force | Out-Null

$GdkPixbufSource = Join-Path $GTKPath "lib\gdk-pixbuf-2.0"
if (Test-Path $GdkPixbufSource) {
    $GdkPixbufDest = Join-Path $LibDir "gdk-pixbuf-2.0"
    Copy-Item $GdkPixbufSource $GdkPixbufDest -Recurse -Force -ErrorAction SilentlyContinue
}

$GtkImmodulesSource = Join-Path $GTKPath "lib\gtk-3.0\3.0.0\immodules"
if (Test-Path $GtkImmodulesSource) {
    $GtkModulesDir = Join-Path $LibDir "gtk-3.0\3.0.0"
    New-Item -ItemType Directory -Path $GtkModulesDir -Force | Out-Null
    $GtkModulesDest = Join-Path $GtkModulesDir "immodules"
    Copy-Item $GtkImmodulesSource $GtkModulesDest -Recurse -Force -ErrorAction SilentlyContinue
}

Write-Host "Creating launcher script..." -ForegroundColor Green
$LauncherPath = Join-Path $PackageDir "DiskAtlas.bat"
$LauncherContent = @"
@echo off
setlocal

:: Set GTK environment variables
set GTK_PATH=%~dp0
set GDK_PIXBUF_MODULE_FILE=%~dp0lib\gdk-pixbuf-2.0\2.10.0\loaders.cache
set GTK_IM_MODULE_FILE=%~dp0lib\gtk-3.0\3.0.0\immodules.cache
set GTK_THEME=Adwaita

:: Launch DiskAtlas
start "" "%~dp0diskatlas.exe"
"@
Set-Content -Path $LauncherPath -Value $LauncherContent

Write-Host "Creating portable README..." -ForegroundColor Green
$ReadmePortablePath = Join-Path $PackageDir "README_PORTABLE.txt"
$ReadmeContent = @"
DiskAtlas Portable v$Version
=================================

This is a portable version of DiskAtlas that includes all
required GTK runtime dependencies.

To run DiskAtlas:
1. Double-click DiskAtlas.bat (recommended)
2. Or double-click diskatlas.exe directly

The .bat launcher sets up the proper GTK environment.

No installation required - just extract and run!
"@
Set-Content -Path $ReadmePortablePath -Value $ReadmeContent

Write-Host "Creating ZIP archive..." -ForegroundColor Green
$ZipName = "diskatlas-$Version-win64.zip"
$ZipPath = Join-Path $OutputDir $ZipName

if (Test-Path $ZipPath) {
    Remove-Item $ZipPath -Force
}

try {
    Compress-Archive -Path "$PackageDir\*" -DestinationPath $ZipPath -Force

    Write-Host ""
    Write-Host "========================================" -ForegroundColor Green
    Write-Host "Portable package created successfully!" -ForegroundColor Green
    Write-Host "========================================" -ForegroundColor Green
    Write-Host "Location: $ZipPath" -ForegroundColor Cyan

    $ZipFile = Get-Item $ZipPath
    $SizeInMB = [math]::Round($ZipFile.Length / 1MB, 2)
    Write-Host "Size: $SizeInMB MB" -ForegroundColor Cyan

    Write-Host ""
    Write-Host "The package includes:" -ForegroundColor Yellow
    Write-Host "- DiskAtlas executable and diskatlas_core.dll" -ForegroundColor White
    Write-Host "- All GTK runtime dependencies" -ForegroundColor White
    Write-Host "- libupdate (update.dll + updater.exe) when built" -ForegroundColor White
    Write-Host "- Launcher script with environment setup" -ForegroundColor White
    Write-Host "- Documentation" -ForegroundColor White
    Write-Host ""

} catch {
    Write-Host "Error creating ZIP archive: $_" -ForegroundColor Red
    Write-Host "Package folder created at: $PackageDir" -ForegroundColor Yellow
    Write-Host "You can manually zip the folder." -ForegroundColor Yellow
}

Write-Host "Done!" -ForegroundColor Green
