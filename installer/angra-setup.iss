#define AppName "ANGRA Acrobat"
#define AppVersion "0.1.0"
#define AppExe "ANGRA.exe"

[Setup]
AppId={{6F0E7C52-3A9B-4D1E-8A75-2C4B9D0E1F23}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher=ANGRA Acrobat Contributors
DefaultDirName={autopf}\ANGRA Acrobat
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
OutputDir=..\dist\windows
OutputBaseFilename=ANGRA-Acrobat-Setup-x64
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
LicenseFile=..\LICENSE
UninstallDisplayIcon={app}\{#AppExe}
DisableProgramGroupPage=yes
; Wizard window and Add/Remove Programs entry, not just installed shortcuts.
SetupIconFile=..\resources\icon.ico

[Tasks]
Name: desktopicon; Description: "Create a &desktop shortcut"; Flags: unchecked
Name: pdfassoc; Description: "Associate .pdf files with {#AppName}"; Flags: unchecked

[Files]
Source: "..\build\ANGRA.exe"; DestDir: "{app}"
Source: "..\build\deploy\*"; DestDir: "{app}"; Flags: recursesubdirs
Source: "..\build\pdfium.dll"; DestDir: "{app}"
Source: "..\build\qpdf*.dll"; DestDir: "{app}"
Source: "..\LICENSE"; DestDir: "{app}"
Source: "..\THIRD_PARTY_NOTICES.md"; DestDir: "{app}"

[Icons]
Name: "{autoprograms}\{#AppName}"; Filename: "{app}\{#AppExe}"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#AppExe}"; Tasks: desktopicon

[Registry]
Root: HKA; Subkey: "Software\Classes\.pdf\OpenWithProgids"; ValueType: string; ValueName: "AngraAcrobat.pdf"; ValueData: ""; Tasks: pdfassoc; Flags: uninsdeletevalue
Root: HKA; Subkey: "Software\Classes\AngraAcrobat.pdf"; ValueType: string; ValueData: "PDF Document"; Tasks: pdfassoc; Flags: uninsdeletekey
; Associated PDFs show the app icon in Explorer, not a blank sheet.
Root: HKA; Subkey: "Software\Classes\AngraAcrobat.pdf\DefaultIcon"; ValueType: string; ValueData: "{app}\{#AppExe},0"; Tasks: pdfassoc
Root: HKA; Subkey: "Software\Classes\AngraAcrobat.pdf\shell\open\command"; ValueType: string; ValueData: """{app}\{#AppExe}"" ""%1"""; Tasks: pdfassoc
