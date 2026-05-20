@echo off
setlocal enabledelayedexpansion

echo ========================================
echo DiskAtlas Complete Build Script
echo ========================================

set VERSION=1.0.0
set APP_NAME=DiskAtlas
set SOURCE_DIR=%~dp0..
set INSTALLER_DIR=%~dp0
set DIST_DIR=%SOURCE_DIR%\dist

echo Building %APP_NAME% v%VERSION% packages...
echo.

:: Create dist directory
if not exist "%DIST_DIR%" mkdir "%DIST_DIR%"

:: Check if executable exists
if not exist "%SOURCE_DIR%\bin\diskatlas.exe" (
    echo Error: diskatlas.exe not found in %SOURCE_DIR%\bin
    echo Please build the application first.
    pause
    exit /b 1
)

if not exist "%SOURCE_DIR%\bin\diskatlas_core.dll" (
    echo Error: diskatlas_core.dll not found in %SOURCE_DIR%\bin
    echo Please build the application first.
    pause
    exit /b 1
)

echo [1/3] Signing executable...
echo =====================================
cd /d "%INSTALLER_DIR%"
if exist "%SOURCE_DIR%\scripts\sign_exe.ps1" if exist "%SOURCE_DIR%\cert\Mechanika Design.pfx" (
    powershell -ExecutionPolicy Bypass -File "%SOURCE_DIR%\scripts\sign_exe.ps1" -ExePath "%SOURCE_DIR%\bin\diskatlas.exe" -CertPath "%SOURCE_DIR%\cert\Mechanika Design.pfx"
    if errorlevel 1 (
        echo Error: Failed to sign executable. Build aborted.
        pause
        exit /b 1
    )
) else (
    echo Skipping code signing ^(sign_exe.ps1 or certificate not found^).
)

echo.
echo [2/3] Creating portable package...
echo =====================================
call "%INSTALLER_DIR%\create_portable_package.bat"
if errorlevel 1 (
    echo Error creating portable package
    pause
    exit /b 1
)

echo.
echo [3/3] Creating Inno Setup installer...
echo =====================================
cd /d "%INSTALLER_DIR%"
powershell -ExecutionPolicy Bypass -File "build_inno.ps1" -Version "%VERSION%"
if errorlevel 1 (
    echo Error creating Inno Setup installer
    pause
    exit /b 1
)

echo.
echo ========================================
echo Build Summary
echo ========================================

if exist "%DIST_DIR%\diskatlas-%VERSION%-win64.zip" (
    echo Portable Package: %DIST_DIR%\diskatlas-%VERSION%-win64.zip
    for %%i in ("%DIST_DIR%\diskatlas-%VERSION%-win64.zip") do echo   Size: %%~zi bytes
) else (
    echo Portable Package: Failed
)

if exist "%DIST_DIR%\diskatlas-%VERSION%-win64-setup.exe" (
    echo Inno Setup Installer: %DIST_DIR%\diskatlas-%VERSION%-win64-setup.exe
    for %%i in ("%DIST_DIR%\diskatlas-%VERSION%-win64-setup.exe") do echo   Size: %%~zi bytes
) else (
    echo Inno Setup Installer: Not created
)

echo.
echo All available packages have been created in: %DIST_DIR%
echo.

echo Opening distribution folder...
explorer "%DIST_DIR%"

echo Build process completed!
pause
