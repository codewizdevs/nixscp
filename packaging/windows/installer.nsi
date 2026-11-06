; NixSCP installer for Windows (NSIS 3, built on Linux by packaging/build-windows.sh)
;   makensis -DVERSION=1.0.3 -DSTAGE=<portable folder> -DOUTFILE=<setup.exe> -DUNINSTALL_LIST=<list.nsh> installer.nsi
Unicode true
SetCompressor /SOLID lzma
RequestExecutionLevel admin
ManifestDPIAware true

!include "MUI2.nsh"
!include "x64.nsh"

Name "NixSCP ${VERSION}"
OutFile "${OUTFILE}"
InstallDir "$PROGRAMFILES64\NixSCP"
InstallDirRegKey HKLM "Software\NixSCP" "InstallDir"
BrandingText "NixSCP ${VERSION}"

VIProductVersion "${VERSION}.0"
VIAddVersionKey "ProductName" "NixSCP"
VIAddVersionKey "CompanyName" "CodeWiz"
VIAddVersionKey "FileDescription" "NixSCP Setup"
VIAddVersionKey "FileVersion" "${VERSION}"
VIAddVersionKey "ProductVersion" "${VERSION}"
VIAddVersionKey "LegalCopyright" "Copyright (c) 2026 Dragan Plavsic, CodeWiz. GPL-3.0."

!define MUI_ICON "${STAGE}\nixscp.ico"
!define MUI_UNICON "${STAGE}\nixscp.ico"
!define MUI_ABORTWARNING
!define MUI_FINISHPAGE_RUN
!define MUI_FINISHPAGE_RUN_TEXT "Start NixSCP"
!define MUI_FINISHPAGE_RUN_FUNCTION StartAsUser

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "${STAGE}\LICENSE.txt"
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

!define ENVIRONMENT_KEY "SYSTEM\CurrentControlSet\Control\Session Manager\Environment"

; Started from here NixSCP would run with the rights of the installer: a server
; would then talk to an administrator's process. The shell starts it as the user.
Function StartAsUser
  Exec '"$WINDIR\explorer.exe" "$INSTDIR\nixscp.exe"'
FunctionEnd

Function .onInit
  ${IfNot} ${RunningX64}
    MessageBox MB_ICONSTOP "NixSCP needs a 64-bit version of Windows."
    Abort
  ${EndIf}
  SetRegView 64
FunctionEnd

Function un.onInit
  SetRegView 64
FunctionEnd

Section "NixSCP" SecMain
  SectionIn RO
  ; A folder of its own, whatever was chosen: the uninstaller removes it when empty
  StrCpy $0 "$INSTDIR" 7 -7
  ${If} $0 != "\NixSCP"
    StrCpy $INSTDIR "$INSTDIR\NixSCP"
  ${EndIf}
  SetOutPath "$INSTDIR"
  ; Program Files is closed to users, a folder like C:\NixSCP is not, and a DLL
  ; placed there would run for everyone who starts NixSCP. Before anything is copied
  nsExec::ExecToLog '"$SYSDIR\icacls.exe" "$INSTDIR" /inheritance:r /grant:r *S-1-5-18:(OI)(CI)F *S-1-5-32-544:(OI)(CI)F *S-1-5-32-545:(OI)(CI)RX /T /C /Q'
  Pop $0
  ${If} $0 != 0
    MessageBox MB_ICONEXCLAMATION|MB_OK "Cannot restrict the permissions of $INSTDIR. Other users of this computer may be able to change NixSCP." /SD IDOK
  ${EndIf}
  File /r "${STAGE}\*.*"
  WriteRegStr HKLM "Software\NixSCP" "InstallDir" "$INSTDIR"
  WriteUninstaller "$INSTDIR\uninstall.exe"
  ; Apps & features
  !define UNINSTALL_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\NixSCP"
  WriteRegStr HKLM "${UNINSTALL_KEY}" "DisplayName" "NixSCP"
  WriteRegStr HKLM "${UNINSTALL_KEY}" "DisplayVersion" "${VERSION}"
  WriteRegStr HKLM "${UNINSTALL_KEY}" "Publisher" "CodeWiz"
  WriteRegStr HKLM "${UNINSTALL_KEY}" "URLInfoAbout" "https://nixscp.com/"
  WriteRegStr HKLM "${UNINSTALL_KEY}" "DisplayIcon" "$INSTDIR\nixscp.exe"
  WriteRegStr HKLM "${UNINSTALL_KEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKLM "${UNINSTALL_KEY}" "UninstallString" '"$INSTDIR\uninstall.exe"'
  WriteRegDWORD HKLM "${UNINSTALL_KEY}" "NoModify" 1
  WriteRegDWORD HKLM "${UNINSTALL_KEY}" "NoRepair" 1
  CreateShortcut "$SMPROGRAMS\NixSCP.lnk" "$INSTDIR\nixscp.exe"
SectionEnd

Section /o "Desktop shortcut" SecDesktop
  CreateShortcut "$DESKTOP\NixSCP.lnk" "$INSTDIR\nixscp.exe"
SectionEnd

Section "Add nixscp-cli to PATH" SecPath
  ; for the current machine; EnVar-free: through the registry and a broadcast.
  ; Path can be longer than a string of NSIS (NSIS_MAX_STRLEN): ReadRegStr then
  ; gives nothing, and writing that back would wipe the Path of the computer
  ReadRegStr $2 HKLM "Software\NixSCP" "AddedToPath"
  ${If} $2 != "1"
    ClearErrors
    ReadRegStr $0 HKLM "${ENVIRONMENT_KEY}" "Path"
    StrCpy $1 "$0;$INSTDIR"
    StrLen $3 "$1"
    ${If} ${Errors}
    ${OrIf} $0 == ""
    ${OrIf} $3 >= ${NSIS_MAX_STRLEN}
      MessageBox MB_ICONINFORMATION|MB_OK "The Path of this computer is too long to change here. To run nixscp-cli from any command prompt, add $INSTDIR to it by hand." /SD IDOK
    ${Else}
      WriteRegExpandStr HKLM "${ENVIRONMENT_KEY}" "Path" "$1"
      WriteRegStr HKLM "Software\NixSCP" "AddedToPath" "1"
      SendMessage ${HWND_BROADCAST} ${WM_WININICHANGE} 0 "STR:Environment" /TIMEOUT=2000
    ${EndIf}
  ${EndIf}
SectionEnd

!insertmacro MUI_FUNCTION_DESCRIPTION_BEGIN
  !insertmacro MUI_DESCRIPTION_TEXT ${SecMain} "NixSCP and the nixscp-cli scripting client."
  !insertmacro MUI_DESCRIPTION_TEXT ${SecDesktop} "A NixSCP shortcut on the desktop."
  !insertmacro MUI_DESCRIPTION_TEXT ${SecPath} "Run nixscp-cli from any command prompt."
!insertmacro MUI_FUNCTION_DESCRIPTION_END

; The URL handler of this user, while it still is NixSCP: one that outlives the
; program runs whatever appears at that path later
!macro RemoveUrlHandler SCHEME
  ReadRegStr $0 HKCU "Software\Classes\${SCHEME}\shell\open\command" ""
  ${If} $0 == '"$INSTDIR\nixscp.exe" /unsafe "%1"'
    DeleteRegKey HKCU "Software\Classes\${SCHEME}"
  ${EndIf}
!macroend

Section "Uninstall"
  ; The configuration (%APPDATA%) stays, as with WinSCP
  !insertmacro RemoveUrlHandler "sftp"
  !insertmacro RemoveUrlHandler "scp"
  !insertmacro RemoveUrlHandler "ftp"
  !insertmacro RemoveUrlHandler "ftps"
  !insertmacro RemoveUrlHandler "ftpes"
  !insertmacro RemoveUrlHandler "dav"
  !insertmacro RemoveUrlHandler "davs"
  !insertmacro RemoveUrlHandler "s3"
  ; What was installed, by name: the folder was chosen by the user, and may
  ; hold other files. Never "RMDir /r" on it
  !include "${UNINSTALL_LIST}"
  Delete "$INSTDIR\uninstall.exe"
  RMDir "$INSTDIR"
  Delete "$SMPROGRAMS\NixSCP.lnk"
  Delete "$DESKTOP\NixSCP.lnk"
  ReadRegStr $0 HKLM "Software\NixSCP" "AddedToPath"
  ${If} $0 == "1"
    ReadRegStr $1 HKLM "${ENVIRONMENT_KEY}" "Path"
    ; remove ";<install dir>" (added at the end)
    StrLen $2 ";$INSTDIR"
    StrLen $3 $1
    IntOp $4 $3 - $2
    StrCpy $5 $1 "" $4
    ${If} $5 == ";$INSTDIR"
      StrCpy $1 $1 $4
      WriteRegExpandStr HKLM "${ENVIRONMENT_KEY}" "Path" "$1"
      SendMessage ${HWND_BROADCAST} ${WM_WININICHANGE} 0 "STR:Environment" /TIMEOUT=2000
    ${EndIf}
  ${EndIf}
  DeleteRegKey HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\NixSCP"
  DeleteRegKey HKLM "Software\NixSCP"
SectionEnd
