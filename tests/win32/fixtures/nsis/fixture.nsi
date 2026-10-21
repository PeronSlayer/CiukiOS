; Authored period installer; compiled only by config/nsis.json's NSIS 2.51.
!include "FileFunc.nsh"
Name "Ciuki Installer Fixture"
OutFile "ciuki-installer-fixture.exe"
InstallDir "$PROGRAMFILES\Ciuki Installer Fixture"
RequestExecutionLevel user
SetCompressor zlib
SetDateSave off
ShowInstDetails show
ShowUninstDetails show
AutoCloseWindow true
Page components
Page directory
Page instfiles
UninstPage uninstConfirm
UninstPage instfiles
Var cancel_test
Var restart_test

Function .onInit
  ${GetParameters} $0
  ClearErrors
  ${GetOptions} $0 "/CANCEL" $1
  IfErrors +2 0
  StrCpy $cancel_test 1
  ClearErrors
  ${GetOptions} $0 "/RESTART" $1
  IfErrors +2 0
  StrCpy $restart_test 1
FunctionEnd

Function Rollback
  SetOutPath "$TEMP"
  Delete "$DESKTOP\Ciuki Installer Fixture.lnk"
  Delete "$SMPROGRAMS\Ciuki Installer Fixture\Readme.lnk"
  RMDir "$SMPROGRAMS\Ciuki Installer Fixture"
  Delete "$INSTDIR\payload.txt"
  Delete "$INSTDIR\child.exe"
  Delete "$INSTDIR\uninstall.exe"
  RMDir "$INSTDIR"
  DeleteRegKey HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\CiukiInstallerFixture"
  DeleteRegValue HKLM "Software\Microsoft\Windows\CurrentVersion\RunOnce" "CiukiInstallerFixture"
FunctionEnd

Function .onUserAbort
  Call Rollback
FunctionEnd

Section "Fixture"
  SetOutPath "$INSTDIR"
  File "payload.txt"
  ; Exercise rollback after extraction has actually created a partial tree.
  StrCmp $cancel_test 1 0 +5
    Call .onUserAbort
    SetErrorLevel 1
    Quit
    Nop
  File "child.exe"
  WriteUninstaller "$INSTDIR\uninstall.exe"
  CreateDirectory "$SMPROGRAMS\Ciuki Installer Fixture"
  CreateShortCut "$SMPROGRAMS\Ciuki Installer Fixture\Readme.lnk" "$INSTDIR\payload.txt" "" "$INSTDIR\child.exe" 0 SW_SHOWNORMAL "" "Ciuki Installer Fixture"
  CreateShortCut "$DESKTOP\Ciuki Installer Fixture.lnk" "$INSTDIR\payload.txt" "" "$INSTDIR\child.exe" 0 SW_SHOWNORMAL "" "Ciuki Installer Fixture"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\CiukiInstallerFixture" "DisplayName" "Ciuki Installer Fixture"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\CiukiInstallerFixture" "UninstallString" '$\"$INSTDIR\uninstall.exe$\"'
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\CiukiInstallerFixture" "DisplayVersion" "1.0"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\CiukiInstallerFixture" "Publisher" "CiukiOS"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\CiukiInstallerFixture" "InstallLocation" "$INSTDIR"
  StrCmp $restart_test 1 0 done
    WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\RunOnce" "CiukiInstallerFixture" '$\"$INSTDIR\child.exe$\" /restart-child'
    FileOpen $0 "$TEMP\CiukiInstallerFixture.remove" w
    FileWrite $0 "authored restart deletion$\r$\n"
    FileClose $0
    ; Windows 9x: the installer writes WININIT.INI, not the NT rename queue.
    WriteINIStr "$WINDIR\WININIT.INI" "rename" "NUL" "$TEMP\CiukiInstallerFixture.remove"
    SetRebootFlag true
  done:
SectionEnd

Section "Uninstall"
  SetOutPath "$TEMP"
  Delete "$DESKTOP\Ciuki Installer Fixture.lnk"
  Delete "$SMPROGRAMS\Ciuki Installer Fixture\Readme.lnk"
  RMDir "$SMPROGRAMS\Ciuki Installer Fixture"
  Delete "$INSTDIR\payload.txt"
  Delete "$INSTDIR\child.exe"
  DeleteRegKey HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\CiukiInstallerFixture"
  DeleteRegValue HKLM "Software\Microsoft\Windows\CurrentVersion\RunOnce" "CiukiInstallerFixture"
  ; The controller runs a temporary copy with _?=$INSTDIR and waits for it.
  Delete "$INSTDIR\uninstall.exe"
  RMDir "$INSTDIR"
  ; Explicit 9x delayed cleanup of the running temporary copy.
  WriteINIStr "$WINDIR\WININIT.INI" "rename" "NUL" "$EXEPATH"
SectionEnd
