; DiskAtlas Inno Setup Script
; This script creates a professional installer with multiple screens

#define MyAppName "DiskAtlas"
#define MyAppVersion "1.0.0"
#define MyAppPublisher "Mechanika Design"
#define MyAppURL "https://software.mechanikadesign.com/software/diskatlas"
#define MyAppExeName "diskatlas.exe"
#define MyAppDescription "A cross-platform disk space analyzer with treemap visualization"

[Setup]
; NOTE: The value of AppId uniquely identifies this application.
AppId={{B8E4D5C6-F7A8-4901-BCDE-F12345678901}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName={#MyAppName} {#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}
AppUpdatesURL={#MyAppURL}
DefaultDirName={autopf}\{#MyAppPublisher}\{#MyAppName}
DefaultGroupName={#MyAppName}
AllowNoIcons=yes
LicenseFile=license.txt
InfoBeforeFile=readme_before.txt
InfoAfterFile=readme_after.txt
OutputDir=..\dist
OutputBaseFilename=diskatlas-{#MyAppVersion}-win64-setup
SetupIconFile=..\resources\app-icon.ico
Compression=lzma
SolidCompression=yes
WizardStyle=modern
WizardSizePercent=120
ShowLanguageDialog=no
PrivilegesRequired=admin
ArchitecturesAllowed=x64
ArchitecturesInstallIn64BitMode=x64

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Types]
Name: "full"; Description: "Full installation"
Name: "minimal"; Description: "Minimal installation"
Name: "custom"; Description: "Custom installation"; Flags: iscustom

[Components]
Name: "main"; Description: "Core application files"; Types: full minimal custom; Flags: fixed
Name: "gtk"; Description: "GTK Runtime Libraries"; Types: full minimal custom; Flags: fixed
Name: "shortcuts"; Description: "Desktop and Start Menu shortcuts"; Types: full custom
Name: "shortcuts\desktop"; Description: "Desktop shortcut"; Types: full custom
Name: "shortcuts\startmenu"; Description: "Start Menu shortcut"; Types: full minimal custom; Flags: fixed

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Components: shortcuts\desktop
Name: "quicklaunchicon"; Description: "{cm:CreateQuickLaunchIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
; Main application files
Source: "..\dist\DiskAtlas\diskatlas.exe"; DestDir: "{app}"; Flags: ignoreversion; Components: main
Source: "..\dist\DiskAtlas\diskatlas_core.dll"; DestDir: "{app}"; Flags: ignoreversion; Components: main
Source: "..\dist\DiskAtlas\DiskAtlas.bat"; DestDir: "{app}"; Flags: ignoreversion; Components: main
Source: "..\dist\DiskAtlas\readme.txt"; DestDir: "{app}"; Flags: ignoreversion; Components: main

; GTK Runtime Libraries - Core GTK DLLs
Source: "..\dist\DiskAtlas\libgtk-3-0.dll"; DestDir: "{app}"; Flags: ignoreversion; Components: gtk
Source: "..\dist\DiskAtlas\libgdk-3-0.dll"; DestDir: "{app}"; Flags: ignoreversion; Components: gtk
Source: "..\dist\DiskAtlas\libgdk_pixbuf-2.0-0.dll"; DestDir: "{app}"; Flags: ignoreversion; Components: gtk

; GLib DLLs
Source: "..\dist\DiskAtlas\libglib-2.0-0.dll"; DestDir: "{app}"; Flags: ignoreversion; Components: gtk
Source: "..\dist\DiskAtlas\libgobject-2.0-0.dll"; DestDir: "{app}"; Flags: ignoreversion; Components: gtk
Source: "..\dist\DiskAtlas\libgio-2.0-0.dll"; DestDir: "{app}"; Flags: ignoreversion; Components: gtk
Source: "..\dist\DiskAtlas\libgmodule-2.0-0.dll"; DestDir: "{app}"; Flags: ignoreversion; Components: gtk

; Cairo and Pango DLLs
Source: "..\dist\DiskAtlas\libcairo-2.dll"; DestDir: "{app}"; Flags: ignoreversion; Components: gtk
Source: "..\dist\DiskAtlas\libcairo-gobject-2.dll"; DestDir: "{app}"; Flags: ignoreversion; Components: gtk
Source: "..\dist\DiskAtlas\libpango-1.0-0.dll"; DestDir: "{app}"; Flags: ignoreversion; Components: gtk
Source: "..\dist\DiskAtlas\libpangowin32-1.0-0.dll"; DestDir: "{app}"; Flags: ignoreversion; Components: gtk
Source: "..\dist\DiskAtlas\libpangocairo-1.0-0.dll"; DestDir: "{app}"; Flags: ignoreversion; Components: gtk
Source: "..\dist\DiskAtlas\libpangoft2-1.0-0.dll"; DestDir: "{app}"; Flags: ignoreversion; Components: gtk

; Font and text rendering
Source: "..\dist\DiskAtlas\libfontconfig-1.dll"; DestDir: "{app}"; Flags: ignoreversion; Components: gtk
Source: "..\dist\DiskAtlas\libfreetype-6.dll"; DestDir: "{app}"; Flags: ignoreversion; Components: gtk
Source: "..\dist\DiskAtlas\libfribidi-0.dll"; DestDir: "{app}"; Flags: ignoreversion; Components: gtk
Source: "..\dist\DiskAtlas\libharfbuzz-0.dll"; DestDir: "{app}"; Flags: ignoreversion; Components: gtk
Source: "..\dist\DiskAtlas\libgraphite2.dll"; DestDir: "{app}"; Flags: ignoreversion; Components: gtk

; Image libraries
Source: "..\dist\DiskAtlas\libpng16-16.dll"; DestDir: "{app}"; Flags: ignoreversion; Components: gtk
Source: "..\dist\DiskAtlas\libpixman-1-0.dll"; DestDir: "{app}"; Flags: ignoreversion; Components: gtk

; OpenGL and graphics
Source: "..\dist\DiskAtlas\libepoxy-0.dll"; DestDir: "{app}"; Flags: ignoreversion; Components: gtk
Source: "..\dist\DiskAtlas\libatk-1.0-0.dll"; DestDir: "{app}"; Flags: ignoreversion; Components: gtk

; Compression and utility libraries
Source: "..\dist\DiskAtlas\zlib1.dll"; DestDir: "{app}"; Flags: ignoreversion; Components: gtk
Source: "..\dist\DiskAtlas\libintl-8.dll"; DestDir: "{app}"; Flags: ignoreversion; Components: gtk
Source: "..\dist\DiskAtlas\libiconv-2.dll"; DestDir: "{app}"; Flags: ignoreversion; Components: gtk
Source: "..\dist\DiskAtlas\libffi-8.dll"; DestDir: "{app}"; Flags: ignoreversion; Components: gtk
Source: "..\dist\DiskAtlas\libpcre2-8-0.dll"; DestDir: "{app}"; Flags: ignoreversion; Components: gtk

; MinGW runtime
Source: "..\dist\DiskAtlas\libgcc_s_seh-1.dll"; DestDir: "{app}"; Flags: ignoreversion; Components: gtk
Source: "..\dist\DiskAtlas\libstdc++-6.dll"; DestDir: "{app}"; Flags: ignoreversion; Components: gtk
Source: "..\dist\DiskAtlas\libwinpthread-1.dll"; DestDir: "{app}"; Flags: ignoreversion; Components: gtk

; Additional dependencies
Source: "..\dist\DiskAtlas\libdatrie-1.dll"; DestDir: "{app}"; Flags: ignoreversion; Components: gtk
Source: "..\dist\DiskAtlas\libbz2-1.dll"; DestDir: "{app}"; Flags: ignoreversion; Components: gtk
Source: "..\dist\DiskAtlas\libexpat-1.dll"; DestDir: "{app}"; Flags: ignoreversion; Components: gtk
Source: "..\dist\DiskAtlas\libbrotlicommon.dll"; DestDir: "{app}"; Flags: ignoreversion; Components: gtk
Source: "..\dist\DiskAtlas\libbrotlidec.dll"; DestDir: "{app}"; Flags: ignoreversion; Components: gtk
Source: "..\dist\DiskAtlas\libthai-0.dll"; DestDir: "{app}"; Flags: ignoreversion; Components: gtk

; libupdate (automatic updates)
Source: "..\dist\DiskAtlas\update.dll"; DestDir: "{app}"; Flags: ignoreversion; Components: main
Source: "..\dist\DiskAtlas\updater.exe"; DestDir: "{app}"; Flags: ignoreversion; Components: main

; Copy lib and share folders recursively
Source: "..\dist\DiskAtlas\lib\*"; DestDir: "{app}\lib"; Flags: ignoreversion recursesubdirs createallsubdirs; Components: gtk
Source: "..\dist\DiskAtlas\share\*"; DestDir: "{app}\share"; Flags: ignoreversion recursesubdirs createallsubdirs; Components: gtk

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Components: shortcuts\startmenu
Name: "{group}\Uninstall {#MyAppName}"; Filename: "{uninstallexe}"; Components: shortcuts\startmenu
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon
Name: "{userappdata}\Microsoft\Internet Explorer\Quick Launch\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: quicklaunchicon

[Registry]
Root: HKCU; Subkey: "Software\{#MyAppPublisher}\{#MyAppName}"; ValueType: string; ValueName: "InstallPath"; ValueData: "{app}"; Flags: uninsdeletekey
Root: HKCU; Subkey: "Software\{#MyAppPublisher}\{#MyAppName}"; ValueType: string; ValueName: "Version"; ValueData: "{#MyAppVersion}"; Flags: uninsdeletekey

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "{cm:LaunchProgram,{#StringChange(MyAppName, '&', '&&')}}"; Flags: nowait postinstall skipifsilent

[UninstallDelete]
Type: filesandordirs; Name: "{app}"

[Code]
procedure InitializeWizard;
begin
  WizardForm.WelcomeLabel1.Caption := 'Welcome to the ' + '{#MyAppName}' + ' Setup Wizard';
  WizardForm.WelcomeLabel2.Caption := 'This will install ' + '{#MyAppName}' + ' {#MyAppVersion} on your computer.' + #13#10#13#10 +
    '{#MyAppDescription}' + #13#10#13#10 +
    'It is recommended that you close all other applications before continuing.' + #13#10#13#10 +
    'Click Next to continue, or Cancel to exit Setup.';
end;

function InitializeSetup(): Boolean;
begin
  Result := True;
  // Add any custom initialization logic here
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then
  begin
    // Add any post-installation tasks here
  end;
end;
