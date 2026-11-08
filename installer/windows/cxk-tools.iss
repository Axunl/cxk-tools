; ---------------------------------------------------------------------------
; CXK 工具箱 的 Windows 安装器（Inno Setup）。
;
; 编译方式（由 scripts/package.bat 与 CI 调用，不要手工拼参数）：
;   ISCC.exe /DAppVersion=0.1.0 installer\windows\cxk-tools.iss
;
; 前置条件：先跑出 dist\windows-x64\（cmake --install + windeployqt + prune 的产物）。
; SourceDir / OutputDir 都相对本文件，所以调用前把工作目录切到仓库根即可。
;
; 编码：本文件是 UTF-8（无 BOM）。Inno Setup 6 会自动识别合法 UTF-8 脚本；
; 若哪天中文在向导里变乱码，第一件事是把本文件改存成「UTF-8 with BOM」，
; 而不是去改文案。
;
; 体积：Compression=lzma2/ultra64 + SolidCompression 让 Qt 各 DLL 的公共段一起压，
; 比逐文件压小得多；这也是安装器比便携 zip 小一截的主要原因。
; ---------------------------------------------------------------------------

#ifndef AppVersion
  #error AppVersion is required: pass /DAppVersion=<x.y.z> (scripts/package.bat does this)
#endif

#define AppName "CXK 工具箱"
#define AppExe "cxk-tools.exe"
#define AppPublisher "cxk"
#define AppRepoUrl "https://github.com/Axunl/cxk-tools"

; 相对本文件（installer\windows\）的默认值；需要换目录时用 /DSourceDir=/DOutputDir= 覆盖
#ifndef SourceDir
  #define SourceDir "..\..\dist\windows-x64"
#endif
#ifndef OutputDir
  #define OutputDir "..\..\dist"
#endif

[Setup]
; AppId 是「同一个程序」的身份：升级安装靠它覆盖旧版本，绝不能再改。
AppId={{7D3B2C9E-5A41-4F2B-9E63-1C8A4B7D0E52}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher={#AppPublisher}
AppPublisherURL={#AppRepoUrl}
AppSupportURL={#AppRepoUrl}/issues
AppUpdatesURL={#AppRepoUrl}/releases
VersionInfoVersion={#AppVersion}
VersionInfoDescription={#AppName} Setup
DefaultDirName={autopf}\CXK 工具箱
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
AllowNoIcons=yes
PrivilegesRequired=admin
; 允许非管理员用户在向导里改选「仅为当前用户安装」，装到 %LOCALAPPDATA%\Programs
PrivilegesRequiredOverridesAllowed=dialog
MinVersion=10.0
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir={#OutputDir}
OutputBaseFilename=cxk-tools-{#AppVersion}-windows-x64-setup
SetupIconFile=..\..\assets\app.ico
UninstallDisplayIcon={app}\{#AppExe}
UninstallDisplayName={#AppName}
Compression=lzma2/ultra64
SolidCompression=yes
LZMAUseSeparateProcess=yes
WizardStyle=modern
; 更新器用 /SILENT 静默升级，此时靠 Restart Manager 关掉正在运行的旧实例
CloseApplications=yes
RestartApplications=no
; 语言按系统自动选（见 [Languages]），不额外弹选择框
ShowLanguageDialog=no

[Languages]
; 顺序有意义：第一个是「系统语言不匹配时的兜底」。
Name: "chinesesimplified"; MessagesFile: "lang\ChineseSimplified.isl"
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
; staging 已经是最终布局（exe 在根、插件在 plugins\），原样铺开即可。
Source: "{#SourceDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\{#AppName}"; Filename: "{app}\{#AppExe}"
Name: "{group}\{cm:UninstallProgram,{#AppName}}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#AppExe}"; Tasks: desktopicon

[Run]
; 安装完成后启动。刻意不加 skipifsilent：应用内更新就是用 /SILENT 调本安装器的，
; 装完自动把新版本拉起来，用户不用再去点开始菜单。
; 与 src/core/UpdateChecker.cpp 里的静默参数是同一份约定，改一处要改两处。
Filename: "{app}\{#AppExe}"; Description: "{cm:LaunchProgram,{#AppName}}"; Flags: nowait postinstall

; 卸载时不删用户配置：热键、最近使用、窗口位置都在 %LOCALAPPDATA%\cxk\cxk-tools，
; 重装后应当还在。安装目录里的东西由 [Files] 的清单负责清理，无需 [UninstallDelete]。
