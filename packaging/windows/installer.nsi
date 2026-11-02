; NixSCP installer for Windows (NSIS 3, built on Linux by packaging/build-windows.sh)
;   makensis -DVERSION=1.0.3 -DSTAGE=<portable folder> -DOUTFILE=<setup.exe> installer.nsi
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
!define MUI_FINISHPAGE_RUN "$INSTDIR\nixscp.exe"
!define MUI_FINISHPAGE_RUN_TEXT "Start NixSCP"

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "${STAGE}\LICENSE.txt"
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

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
  SetOutPath "$INSTDIR"
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
  ; for the current machine; EnVar-free: through the registry and a broadcast
  ReadRegStr $0 HKLM "SYSTEM\CurrentControlSet\Control\Session Manager\Environment" "Path"
  StrCpy $1 "$0;$INSTDIR"
  WriteRegExpandStr HKLM "SYSTEM\CurrentControlSet\Control\Session Manager\Environment" "Path" "$1"
  WriteRegStr HKLM "Software\NixSCP" "AddedToPath" "1"
  SendMessage ${HWND_BROADCAST} ${WM_WININICHANGE} 0 "STR:Environment" /TIMEOUT=2000
SectionEnd

!insertmacro MUI_FUNCTION_DESCRIPTION_BEGIN
  !insertmacro MUI_DESCRIPTION_TEXT ${SecMain} "NixSCP and the nixscp-cli scripting client."
  !insertmacro MUI_DESCRIPTION_TEXT ${SecDesktop} "A NixSCP shortcut on the desktop."
  !insertmacro MUI_DESCRIPTION_TEXT ${SecPath} "Run nixscp-cli from any command prompt."
!insertmacro MUI_FUNCTION_DESCRIPTION_END

Section "Uninstall"
  ; The configuration (%APPDATA%) stays, as with WinSCP
  RMDir /r "$INSTDIR"
  Delete "$SMPROGRAMS\NixSCP.lnk"
  Delete "$DESKTOP\NixSCP.lnk"
  ReadRegStr $0 HKLM "Software\NixSCP" "AddedToPath"
  ${If} $0 == "1"
    ReadRegStr $1 HKLM "SYSTEM\CurrentControlSet\Control\Session Manager\Environment" "Path"
    ; remove ";<install dir>" (added at the end)
    StrLen $2 ";$INSTDIR"
    StrLen $3 $1
    IntOp $4 $3 - $2
    StrCpy $5 $1 "" $4
    ${If} $5 == ";$INSTDIR"
      StrCpy $1 $1 $4
      WriteRegExpandStr HKLM "SYSTEM\CurrentControlSet\Control\Session Manager\Environment" "Path" "$1"
      SendMessage ${HWND_BROADCAST} ${WM_WININICHANGE} 0 "STR:Environment" /TIMEOUT=2000
    ${EndIf}
  ${EndIf}
  DeleteRegKey HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\NixSCP"
  DeleteRegKey HKLM "Software\NixSCP"
SectionEnd
