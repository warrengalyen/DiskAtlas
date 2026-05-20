@echo off
setlocal enabledelayedexpansion

echo ========================================
echo DiskAtlas Portable Package Creator
echo ========================================

set VERSION=1.0.0
set APP_NAME=DiskAtlas
set SOURCE_DIR=%~dp0..
set DIST_DIR=%SOURCE_DIR%\dist
set PACKAGE_DIR=%DIST_DIR%\%APP_NAME%

echo Creating portable package for %APP_NAME% v%VERSION%...

if not exist "%DIST_DIR%" mkdir "%DIST_DIR%"
if exist "%PACKAGE_DIR%" rmdir /s /q "%PACKAGE_DIR%"
mkdir "%PACKAGE_DIR%"

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

echo Copying main executable and core library...
copy "%SOURCE_DIR%\bin\diskatlas.exe" "%PACKAGE_DIR%\" >nul
copy "%SOURCE_DIR%\bin\diskatlas_core.dll" "%PACKAGE_DIR%\" >nul

echo Copying documentation...
if exist "%SOURCE_DIR%\readme.txt" copy "%SOURCE_DIR%\readme.txt" "%PACKAGE_DIR%\" >nul

echo Copying GTK dependencies...
set GTK_BIN_DIR=C:\msys64\mingw64\bin
if exist "%GTK_BIN_DIR%" (
    copy "%GTK_BIN_DIR%\libcairo-2.dll" "%PACKAGE_DIR%\" >nul 2>&1
    copy "%GTK_BIN_DIR%\libgdk-3-0.dll" "%PACKAGE_DIR%\" >nul 2>&1
    copy "%GTK_BIN_DIR%\libgdk_pixbuf-2.0-0.dll" "%PACKAGE_DIR%\" >nul 2>&1
    copy "%GTK_BIN_DIR%\libgio-2.0-0.dll" "%PACKAGE_DIR%\" >nul 2>&1
    copy "%GTK_BIN_DIR%\libgcc_s_seh-1.dll" "%PACKAGE_DIR%\" >nul 2>&1
    copy "%GTK_BIN_DIR%\libstdc++-6.dll" "%PACKAGE_DIR%\" >nul 2>&1
    copy "%GTK_BIN_DIR%\libglib-2.0-0.dll" "%PACKAGE_DIR%\" >nul 2>&1
    copy "%GTK_BIN_DIR%\libgobject-2.0-0.dll" "%PACKAGE_DIR%\" >nul 2>&1
    copy "%GTK_BIN_DIR%\libgtk-3-0.dll" "%PACKAGE_DIR%\" >nul 2>&1
    copy "%GTK_BIN_DIR%\libfontconfig-1.dll" "%PACKAGE_DIR%\" >nul 2>&1
    copy "%GTK_BIN_DIR%\libfreetype-6.dll" "%PACKAGE_DIR%\" >nul 2>&1
    copy "%GTK_BIN_DIR%\libpixman-1-0.dll" "%PACKAGE_DIR%\" >nul 2>&1
    copy "%GTK_BIN_DIR%\zlib1.dll" "%PACKAGE_DIR%\" >nul 2>&1
    copy "%GTK_BIN_DIR%\libpng16-16.dll" "%PACKAGE_DIR%\" >nul 2>&1
    copy "%GTK_BIN_DIR%\libintl-8.dll" "%PACKAGE_DIR%\" >nul 2>&1
    copy "%GTK_BIN_DIR%\libgmodule-2.0-0.dll" "%PACKAGE_DIR%\" >nul 2>&1
    copy "%GTK_BIN_DIR%\libcairo-gobject-2.dll" "%PACKAGE_DIR%\" >nul 2>&1
    copy "%GTK_BIN_DIR%\libfribidi-0.dll" "%PACKAGE_DIR%\" >nul 2>&1
    copy "%GTK_BIN_DIR%\libepoxy-0.dll" "%PACKAGE_DIR%\" >nul 2>&1
    copy "%GTK_BIN_DIR%\libwinpthread-1.dll" "%PACKAGE_DIR%\" >nul 2>&1
    copy "%GTK_BIN_DIR%\libpcre2-8-0.dll" "%PACKAGE_DIR%\" >nul 2>&1
    copy "%GTK_BIN_DIR%\libffi-8.dll" "%PACKAGE_DIR%\" >nul 2>&1
    copy "%GTK_BIN_DIR%\libpango-1.0-0.dll" "%PACKAGE_DIR%\" >nul 2>&1
    copy "%GTK_BIN_DIR%\libpangocairo-1.0-0.dll" "%PACKAGE_DIR%\" >nul 2>&1
    copy "%GTK_BIN_DIR%\libpangowin32-1.0-0.dll" "%PACKAGE_DIR%\" >nul 2>&1
    copy "%GTK_BIN_DIR%\libatk-1.0-0.dll" "%PACKAGE_DIR%\" >nul 2>&1
    copy "%GTK_BIN_DIR%\libharfbuzz-0.dll" "%PACKAGE_DIR%\" >nul 2>&1
    copy "%GTK_BIN_DIR%\libexpat-1.dll" "%PACKAGE_DIR%\" >nul 2>&1
    copy "%GTK_BIN_DIR%\libbz2-1.dll" "%PACKAGE_DIR%\" >nul 2>&1
    copy "%GTK_BIN_DIR%\libbrotlidec.dll" "%PACKAGE_DIR%\" >nul 2>&1
    copy "%GTK_BIN_DIR%\libiconv-2.dll" "%PACKAGE_DIR%\" >nul 2>&1
    copy "%GTK_BIN_DIR%\libthai-0.dll" "%PACKAGE_DIR%\" >nul 2>&1
    copy "%GTK_BIN_DIR%\libpangoft2-1.0-0.dll" "%PACKAGE_DIR%\" >nul 2>&1
    copy "%GTK_BIN_DIR%\libbrotlicommon.dll" "%PACKAGE_DIR%\" >nul 2>&1
    copy "%GTK_BIN_DIR%\libgraphite2.dll" "%PACKAGE_DIR%\" >nul 2>&1
    copy "%GTK_BIN_DIR%\libdatrie-1.dll" "%PACKAGE_DIR%\" >nul 2>&1
)

if exist "%SOURCE_DIR%\bin\update.dll" (
    copy "%SOURCE_DIR%\bin\update.dll" "%PACKAGE_DIR%\" >nul 2>&1
)
if exist "%SOURCE_DIR%\bin\updater.exe" (
    copy "%SOURCE_DIR%\bin\updater.exe" "%PACKAGE_DIR%\" >nul 2>&1
)

set GTK_SHARE_DIR=C:\msys64\mingw64\share
if exist "%GTK_SHARE_DIR%" (
    echo Copying GTK themes and icons...
    if not exist "%PACKAGE_DIR%\share" mkdir "%PACKAGE_DIR%\share"

    if exist "%GTK_SHARE_DIR%\themes" (
        xcopy "%GTK_SHARE_DIR%\themes" "%PACKAGE_DIR%\share\themes" /e /i /q >nul 2>&1
    )

    if exist "%GTK_SHARE_DIR%\icons\Adwaita" (
        if not exist "%PACKAGE_DIR%\share\icons" mkdir "%PACKAGE_DIR%\share\icons"
        xcopy "%GTK_SHARE_DIR%\icons\Adwaita" "%PACKAGE_DIR%\share\icons\Adwaita" /e /i /q >nul 2>&1
    )

    if exist "%GTK_SHARE_DIR%\glib-2.0\schemas" (
        if not exist "%PACKAGE_DIR%\share\glib-2.0" mkdir "%PACKAGE_DIR%\share\glib-2.0"
        xcopy "%GTK_SHARE_DIR%\glib-2.0\schemas" "%PACKAGE_DIR%\share\glib-2.0\schemas" /e /i /q >nul 2>&1
    )
)

set GTK_LIB_DIR=C:\msys64\mingw64\lib
if exist "%GTK_LIB_DIR%" (
    echo Copying GTK modules...
    if not exist "%PACKAGE_DIR%\lib" mkdir "%PACKAGE_DIR%\lib"

    if exist "%GTK_LIB_DIR%\gdk-pixbuf-2.0" (
        xcopy "%GTK_LIB_DIR%\gdk-pixbuf-2.0" "%PACKAGE_DIR%\lib\gdk-pixbuf-2.0" /e /i /q >nul 2>&1
    )

    if exist "%GTK_LIB_DIR%\gtk-3.0\3.0.0\immodules" (
        if not exist "%PACKAGE_DIR%\lib\gtk-3.0\3.0.0" mkdir "%PACKAGE_DIR%\lib\gtk-3.0\3.0.0"
        xcopy "%GTK_LIB_DIR%\gtk-3.0\3.0.0\immodules" "%PACKAGE_DIR%\lib\gtk-3.0\3.0.0\immodules" /e /i /q >nul 2>&1
    )
)

echo Creating launcher script...
echo @echo off > "%PACKAGE_DIR%\DiskAtlas.bat"
echo setlocal >> "%PACKAGE_DIR%\DiskAtlas.bat"
echo. >> "%PACKAGE_DIR%\DiskAtlas.bat"
echo :: Set GTK environment variables >> "%PACKAGE_DIR%\DiskAtlas.bat"
echo set GTK_PATH=%%~dp0 >> "%PACKAGE_DIR%\DiskAtlas.bat"
echo set GDK_PIXBUF_MODULE_FILE=%%~dp0lib\gdk-pixbuf-2.0\2.10.0\loaders.cache >> "%PACKAGE_DIR%\DiskAtlas.bat"
echo set GTK_IM_MODULE_FILE=%%~dp0lib\gtk-3.0\3.0.0\immodules.cache >> "%PACKAGE_DIR%\DiskAtlas.bat"
echo set GTK_THEME=Adwaita >> "%PACKAGE_DIR%\DiskAtlas.bat"
echo. >> "%PACKAGE_DIR%\DiskAtlas.bat"
echo :: Launch DiskAtlas >> "%PACKAGE_DIR%\DiskAtlas.bat"
echo start "" "%%~dp0diskatlas.exe" >> "%PACKAGE_DIR%\DiskAtlas.bat"

echo Creating portable README...
echo DiskAtlas Portable v%VERSION% > "%PACKAGE_DIR%\README_PORTABLE.txt"
echo ================================= >> "%PACKAGE_DIR%\README_PORTABLE.txt"
echo. >> "%PACKAGE_DIR%\README_PORTABLE.txt"
echo This is a portable version of DiskAtlas that includes all >> "%PACKAGE_DIR%\README_PORTABLE.txt"
echo required GTK runtime dependencies and update check functionality. >> "%PACKAGE_DIR%\README_PORTABLE.txt"
echo. >> "%PACKAGE_DIR%\README_PORTABLE.txt"
echo To run DiskAtlas: >> "%PACKAGE_DIR%\README_PORTABLE.txt"
echo 1. Double-click DiskAtlas.bat (recommended) >> "%PACKAGE_DIR%\README_PORTABLE.txt"
echo 2. Or double-click diskatlas.exe directly >> "%PACKAGE_DIR%\README_PORTABLE.txt"
echo. >> "%PACKAGE_DIR%\README_PORTABLE.txt"
echo The .bat launcher sets up the proper GTK environment. >> "%PACKAGE_DIR%\README_PORTABLE.txt"
echo. >> "%PACKAGE_DIR%\README_PORTABLE.txt"
echo No installation required - just extract and run! >> "%PACKAGE_DIR%\README_PORTABLE.txt"

echo Creating ZIP archive...
set ZIP_NAME=diskatlas-%VERSION%-win64.zip
if exist "%DIST_DIR%\%ZIP_NAME%" del "%DIST_DIR%\%ZIP_NAME%"

powershell -command "if (Test-Path '%PACKAGE_DIR%') { Compress-Archive -Path '%PACKAGE_DIR%\*' -DestinationPath '%DIST_DIR%\%ZIP_NAME%' -Force }" >nul 2>&1

if exist "%DIST_DIR%\%ZIP_NAME%" (
    echo.
    echo ========================================
    echo Portable package created successfully!
    echo ========================================
    echo Location: %DIST_DIR%\%ZIP_NAME%
    for %%i in ("%DIST_DIR%\%ZIP_NAME%") do echo Size: %%~zi bytes
    echo.
) else (
    echo.
    echo Package folder created at: %PACKAGE_DIR%
    echo Note: ZIP creation failed. You can manually zip the folder.
    echo.
)

echo Done!
pause
