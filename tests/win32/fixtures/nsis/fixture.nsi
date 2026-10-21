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
Var cancel_late
Var restart_test
Var own_dir
Var own_menu
Var own_payload
Var own_child
Var own_uninstaller
Var own_desktop
Var own_link
Var own_key
Var own_runkey
Var own_DisplayName
Var own_DisplayVersion
Var own_Publisher
Var own_InstallLocation
Var own_UninstallString
Var own_runonce

!define UK "Software\Microsoft\Windows\CurrentVersion\Uninstall\CiukiInstallerFixture"
!define RK "Software\Microsoft\Windows\CurrentVersion\RunOnce"

Function .onInit
  ${GetParameters} $0
  ClearErrors
  ${GetOptions} $0 "/CANCEL" $1
  IfErrors +2 0
  StrCpy $cancel_test 1
  ClearErrors
  ${GetOptions} $0 "/LATE" $1
  IfErrors +2 0
  StrCpy $cancel_late 1
  ClearErrors
  ${GetOptions} $0 "/RESTART" $1
  IfErrors +2 0
  StrCpy $restart_test 1
FunctionEnd

; Enumerate names, rather than ReadRegStr: an existing value of any type
; belongs to the previous installation and must not be overwritten.
Function ValueExists
  StrCpy $2 0
  StrCpy $3 0
  loop:
    ClearErrors
    EnumRegValue $4 HKLM $0 $3
    IfErrors done
    StrCmp $4 $1 found
    IntOp $3 $3 + 1
    Goto loop
  found:
    StrCpy $2 1
  done:
FunctionEnd

!macro Checked instruction
  ClearErrors
  ${instruction}
  IfErrors failed
!macroend

!macro OwnedFile flag instruction
  StrCpy $${flag} 1
  !insertmacro Checked `${instruction}`
!macroend

!macro Registry name value
  StrCpy $0 "${UK}"
  StrCpy $1 "${name}"
  Call ValueExists
  StrCmp $2 1 skip_${name}
  !insertmacro OwnedFile own_${name} 'WriteRegStr HKLM "${UK}" "${name}" ${value}'
  skip_${name}:
!macroend

Function Rollback
  StrCpy $9 0
  ; Continue after every failed removal and report once at the end.
  SetOutPath "$TEMP"
  StrCmp $own_desktop 1 0 undo_0
    ClearErrors
    Delete "$DESKTOP\Ciuki Installer Fixture.lnk"
    IfErrors 0 undo_0
    StrCpy $9 1
  undo_0:
  StrCmp $own_link 1 0 undo_1
    ClearErrors
    Delete "$SMPROGRAMS\Ciuki Installer Fixture\Readme.lnk"
    IfErrors 0 undo_1
    StrCpy $9 1
  undo_1:
  StrCmp $own_menu 1 0 undo_2
    ClearErrors
    RMDir "$SMPROGRAMS\Ciuki Installer Fixture"
    IfErrors 0 undo_2
    StrCpy $9 1
  undo_2:
  StrCmp $own_payload 1 0 undo_3
    ClearErrors
    Delete "$INSTDIR\payload.txt"
    IfErrors 0 undo_3
    StrCpy $9 1
  undo_3:
  StrCmp $own_child 1 0 undo_4
    ClearErrors
    Delete "$INSTDIR\child.exe"
    IfErrors 0 undo_4
    StrCpy $9 1
  undo_4:
  StrCmp $own_uninstaller 1 0 undo_5
    ClearErrors
    Delete "$INSTDIR\uninstall.exe"
    IfErrors 0 undo_5
    StrCpy $9 1
  undo_5:
  StrCmp $own_dir 1 0 undo_6
    ClearErrors
    RMDir "$INSTDIR"
    IfErrors 0 undo_6
    StrCpy $9 1
  undo_6:
  StrCmp $own_DisplayName 1 0 undo_7
    ClearErrors
    DeleteRegValue HKLM "${UK}" "DisplayName"
    IfErrors 0 undo_7
    StrCpy $9 1
  undo_7:
  StrCmp $own_DisplayVersion 1 0 undo_8
    ClearErrors
    DeleteRegValue HKLM "${UK}" "DisplayVersion"
    IfErrors 0 undo_8
    StrCpy $9 1
  undo_8:
  StrCmp $own_Publisher 1 0 undo_9
    ClearErrors
    DeleteRegValue HKLM "${UK}" "Publisher"
    IfErrors 0 undo_9
    StrCpy $9 1
  undo_9:
  StrCmp $own_InstallLocation 1 0 undo_10
    ClearErrors
    DeleteRegValue HKLM "${UK}" "InstallLocation"
    IfErrors 0 undo_10
    StrCpy $9 1
  undo_10:
  StrCmp $own_UninstallString 1 0 undo_11
    ClearErrors
    DeleteRegValue HKLM "${UK}" "UninstallString"
    IfErrors 0 undo_11
    StrCpy $9 1
  undo_11:
  StrCmp $own_runonce 1 0 undo_12
    ClearErrors
    DeleteRegValue HKLM "${RK}" "CiukiInstallerFixture"
    IfErrors 0 undo_12
    StrCpy $9 1
  undo_12:
  StrCmp $own_key 1 0 undo_13
    ClearErrors
    DeleteRegKey /ifempty HKLM "${UK}"
    IfErrors 0 undo_13
    StrCpy $9 1
  undo_13:
  StrCmp $own_runkey 1 0 undo_14
    ClearErrors
    DeleteRegKey /ifempty HKLM "${RK}"
    IfErrors 0 undo_14
    StrCpy $9 1
  undo_14:
  StrCmp $9 0 +2
    SetErrorLevel 2
FunctionEnd

Function .onUserAbort
  Call Rollback
FunctionEnd

Function .onInstFailed
  Call Rollback
FunctionEnd

Section "Fixture"
  IfFileExists "$INSTDIR" dir_exists
    !insertmacro OwnedFile own_dir 'CreateDirectory "$INSTDIR"'
  dir_exists:
  !insertmacro Checked 'SetOutPath "$INSTDIR"'
  IfFileExists "$INSTDIR\payload.txt" payload_exists
    !insertmacro OwnedFile own_payload 'File "payload.txt"'
  payload_exists:
  ; Early and late cancellation both take the real callback path.
  StrCmp $cancel_test 1 0 early_done
    StrCmp $cancel_late 1 early_done cancel
  early_done:
  IfFileExists "$INSTDIR\child.exe" child_exists
    !insertmacro OwnedFile own_child 'File "child.exe"'
  child_exists:
  IfFileExists "$INSTDIR\uninstall.exe" uninstaller_exists
    !insertmacro OwnedFile own_uninstaller 'WriteUninstaller "$INSTDIR\uninstall.exe"'
  uninstaller_exists:
  IfFileExists "$SMPROGRAMS\Ciuki Installer Fixture" menu_exists
    !insertmacro OwnedFile own_menu 'CreateDirectory "$SMPROGRAMS\Ciuki Installer Fixture"'
  menu_exists:
  IfFileExists "$SMPROGRAMS\Ciuki Installer Fixture\Readme.lnk" link_exists
    !insertmacro OwnedFile own_link 'CreateShortCut "$SMPROGRAMS\Ciuki Installer Fixture\Readme.lnk" "$INSTDIR\payload.txt" "" "$INSTDIR\child.exe" 0 SW_SHOWNORMAL "" "Ciuki Installer Fixture"'
  link_exists:
  IfFileExists "$DESKTOP\Ciuki Installer Fixture.lnk" desktop_exists
    !insertmacro OwnedFile own_desktop 'CreateShortCut "$DESKTOP\Ciuki Installer Fixture.lnk" "$INSTDIR\payload.txt" "" "$INSTDIR\child.exe" 0 SW_SHOWNORMAL "" "Ciuki Installer Fixture"'
  desktop_exists:
  ; Opening a key with EnumRegKey does not create it. A pre-existing empty
  ; key is preserved as well as its values.
  StrCpy $3 0
  key_loop:
    ClearErrors
    EnumRegKey $4 HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall" $3
    IfErrors key_missing
    StrCmp $4 "CiukiInstallerFixture" key_exists
    IntOp $3 $3 + 1
    Goto key_loop
  key_missing:
    StrCpy $own_key 1
  key_exists:
  !insertmacro Registry DisplayName '"Ciuki Installer Fixture"'
  !insertmacro Registry UninstallString '"$\"$INSTDIR\uninstall.exe$\""'
  !insertmacro Registry DisplayVersion '"1.0"'
  !insertmacro Registry Publisher '"CiukiOS"'
  !insertmacro Registry InstallLocation '"$INSTDIR"'
  StrCmp $restart_test 1 0 late_checkpoint
    StrCpy $3 0
    runkey_loop:
      ClearErrors
      EnumRegKey $4 HKLM "Software\Microsoft\Windows\CurrentVersion" $3
      IfErrors runkey_missing
      StrCmp $4 "RunOnce" runkey_exists
      IntOp $3 $3 + 1
      Goto runkey_loop
    runkey_missing:
      StrCpy $own_runkey 1
    runkey_exists:
    StrCpy $0 "${RK}"
    StrCpy $1 "CiukiInstallerFixture"
    Call ValueExists
    StrCmp $2 1 late_checkpoint
    !insertmacro OwnedFile own_runonce `WriteRegStr HKLM "${RK}" "CiukiInstallerFixture" '$\"$INSTDIR\child.exe$\" /restart-child'`
  late_checkpoint:
  StrCmp $cancel_test 1 0 restart
    StrCmp $cancel_late 1 cancel
  restart:
  StrCmp $restart_test 1 0 done
    !insertmacro Checked 'FileOpen $0 "$TEMP\CiukiInstallerFixture.remove" w'
    ClearErrors
    FileWrite $0 "authored restart deletion$\r$\n"
    IfErrors 0 +3
      FileClose $0
      Goto failed
    !insertmacro Checked 'FileClose $0'
    ; Windows 9x: WININIT.INI, not the NT rename queue.
    !insertmacro Checked 'WriteINIStr "$WINDIR\WININIT.INI" "rename" "NUL" "$TEMP\CiukiInstallerFixture.remove"'
    SetRebootFlag true
    Goto done
  cancel:
    SetErrorLevel 1
    Call .onUserAbort
    Quit
  failed:
    SetErrorLevel 2
    Call Rollback
    Quit
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
  ; Both the original engine delegation and explicit _?= copy are tested.
  Delete "$INSTDIR\uninstall.exe"
  RMDir "$INSTDIR"
  ; Explicit 9x delayed cleanup of the running temporary copy.
  ClearErrors
  WriteINIStr "$WINDIR\WININIT.INI" "rename" "NUL" "$EXEPATH"
  IfErrors uninstall_failed
  ; Native NSIS delegation uses a nested ~nsu.tmp directory. Retire the
  ; directory after its executable, using the Win9x DIRNUL section.
  StrCmp $EXEDIR "$WINDIR\TEMP" uninstall_done
  ClearErrors
  WriteINIStr "$WINDIR\WININIT.INI" "DIRNUL" "$EXEDIR" "0"
  IfErrors uninstall_failed uninstall_done
  uninstall_failed:
    SetErrorLevel 2
  uninstall_done:
SectionEnd
