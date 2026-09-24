; The Windows installer, compiled by scripts/package-windows.ps1 with Inno Setup 6:
;
;   ISCC /DAppVersion=2.72.0 /DSourceDir=<staged files> /DOutputDir=<dist> MeshClient.iss
;
; It installs per user, into %LOCALAPPDATA%\Programs\MeshClient, and never asks for elevation.
; That is what lets the in-app updater work: it replaces meshclient.exe in this directory from
; inside the running client, which a Program Files install would refuse without an
; administrator. It is the same trade the handheld makes, where the pak lives on the SD card the
; client can write to.

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#ifndef SourceDir
  #error SourceDir must name the staged files (scripts/package-windows.ps1 sets it)
#endif
#ifndef OutputDir
  #define OutputDir "."
#endif

[Setup]
; Fixed forever: this is how a newer installer recognises the install it is upgrading.
AppId={{AD5C18A6-6AF7-4A46-A357-CA30FBA8D479}
AppName=MeshClient
AppVersion={#AppVersion}
AppVerName=MeshClient {#AppVersion}
AppPublisher=mcereal
AppPublisherURL=https://github.com/mcereal/mesh-client
AppSupportURL=https://github.com/mcereal/mesh-client/issues
AppUpdatesURL=https://github.com/mcereal/mesh-client/releases
DefaultDirName={localappdata}\Programs\MeshClient
DefaultGroupName=MeshClient
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir={#OutputDir}
OutputBaseFilename=MeshClient-windows-x86_64-setup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
LicenseFile={#SourceDir}\licenses\LICENSE-MeshClient.txt
UninstallDisplayName=MeshClient
UninstallDisplayIcon={app}\meshclient.exe
; The executable carries the same icon as a resource, so its shortcuts need none of their own.
SetupIconFile=meshclient.ico
; A running client holds meshclient.exe open; the Restart Manager closes it before the files go.
CloseApplications=yes

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Dirs]
; Where the client keeps its settings and history. With no HOME on Windows, the client falls back
; to the directory it was started in, so the shortcuts start it here and not in {app}. An
; uninstall then leaves a user's data behind, as it should.
Name: "{localappdata}\MeshClient"

[Files]
Source: "{#SourceDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[InstallDelete]
; What an in-app update leaves beside the binary: the one it stepped aside from, and a download
; that never finished.
Type: files; Name: "{app}\meshclient.exe.old"
Type: files; Name: "{app}\meshclient.exe.update"

[UninstallDelete]
Type: files; Name: "{app}\meshclient.exe.old"
Type: files; Name: "{app}\meshclient.exe.update"

[Icons]
; --foreground keeps the window open; without it the client polls once and exits.
Name: "{autoprograms}\MeshClient"; Filename: "{app}\meshclient.exe"; Parameters: "--foreground"; WorkingDir: "{localappdata}\MeshClient"
Name: "{autodesktop}\MeshClient"; Filename: "{app}\meshclient.exe"; Parameters: "--foreground"; WorkingDir: "{localappdata}\MeshClient"; Tasks: desktopicon

[Run]
Filename: "{app}\meshclient.exe"; Parameters: "--foreground"; WorkingDir: "{localappdata}\MeshClient"; Description: "{cm:LaunchProgram,MeshClient}"; Flags: nowait postinstall skipifsilent
