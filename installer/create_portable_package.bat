@echo off
setlocal enabledelayedexpansion

echo ========================================
echo DiskAtlas Portable Package Creator
echo ========================================

set VERSION=1.0.0
set SOURCE_DIR=%~dp0..
set DIST_DIR=%SOURCE_DIR%\dist
set PACKAGE_DIR=%DIST_DIR%\DiskAtlas
set MSYS_BASH=C:\msys64\usr\bin\bash.exe

if not exist "%SOURCE_DIR%\bin\diskatlas.exe" (
    echo Error: diskatlas.exe not found in %SOURCE_DIR%\bin
    echo Please build the application first.
    pause
    exit /b 1
)

if not exist "%MSYS_BASH%" (
    echo Error: MSYS2 not found at %MSYS_BASH%
    echo Install MSYS2 and build with the same environment you use for CMake.
    pause
    exit /b 1
)

echo Bundling portable package via scripts/bundle_portable.sh ...
echo Use the same MSYS2 environment you built with ^(MinGW 64-bit recommended^).
"%MSYS_BASH%" -lc "export MSYSTEM=MINGW64; source /etc/profile; cd '%SOURCE_DIR:\=/%' && ./scripts/bundle_portable.sh"
if errorlevel 1 (
    echo Error: bundle_portable.sh failed.
    pause
    exit /b 1
)

echo Creating ZIP archive...
set ZIP_NAME=diskatlas-%VERSION%-win64.zip
if exist "%DIST_DIR%\%ZIP_NAME%" del "%DIST_DIR%\%ZIP_NAME%"

powershell -command "if (Test-Path '%PACKAGE_DIR%') { Compress-Archive -Path '%PACKAGE_DIR%\*' -DestinationPath '%DIST_DIR%\%ZIP_NAME%' -Force }" >nul 2>&1

if exist "%DIST_DIR%\%ZIP_NAME%" (
    echo.
    echo ========================================
    echo Portable package created successfully!
    echo ========================================
    echo Folder: %PACKAGE_DIR%
    echo ZIP:    %DIST_DIR%\%ZIP_NAME%
    echo.
) else (
    echo.
    echo Package folder: %PACKAGE_DIR%
    echo Note: ZIP creation failed. You can manually zip the folder.
    echo.
)

echo Done!
pause
