; Authored NSIS 2.x fixture. No original application payloads or plugins.
; Compile with the lead's approved, pinned NSIS 2.x compiler before claiming
; engine evidence. A script hash alone is not a compiler provenance pin.
Name "Ciuki Installer Fixture"
OutFile "ciuki-installer-fixture.exe"
InstallDir "$PROGRAMFILES\Ciuki Installer Fixture"
ShowInstDetails show
ShowUninstDetails show
Page components
Page directory
Page instfiles
UninstPage uninstConfirm
UninstPage instfiles

Section "Fixture"
  SetOutPath "$INSTDIR"
  File "payload.txt"
  WriteUninstaller "$INSTDIR\uninstall.exe"
  CreateDirectory "$SMPROGRAMS\Ciuki Installer Fixture"
  CreateShortCut "$SMPROGRAMS\Ciuki Installer Fixture\Readme.lnk" "$INSTDIR\payload.txt" "" "$INSTDIR\uninstall.exe" 0 SW_SHOWNORMAL "" "Ciuki Installer Fixture"
  CreateShortCut "$DESKTOP\Ciuki Installer Fixture.lnk" "$INSTDIR\payload.txt" "" "$INSTDIR\uninstall.exe" 0 SW_SHOWNORMAL "" "Ciuki Installer Fixture"
  ; Registration is the final install step; rollback removes this fixture's
  ; values only. The desktop must not synthesize an Uninstall record.
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\CiukiInstallerFixture" "DisplayName" "Ciuki Installer Fixture"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\CiukiInstallerFixture" "UninstallString" '$"$INSTDIR\uninstall.exe$"'
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\CiukiInstallerFixture" "DisplayVersion" "1.0"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\CiukiInstallerFixture" "Publisher" "CiukiOS"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\CiukiInstallerFixture" "InstallLocation" "$INSTDIR"
SectionEnd

Section /o "Request a virtual restart"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\RunOnce" "CiukiInstallerFixture" '$"$INSTDIR\uninstall.exe$" /restart-fixture'
  SetRebootFlag true
  Reboot
SectionEnd

Function .onUserAbort
  Delete "$DESKTOP\Ciuki Installer Fixture.lnk"
  Delete "$SMPROGRAMS\Ciuki Installer Fixture\Readme.lnk"
  RMDir "$SMPROGRAMS\Ciuki Installer Fixture"
  Delete "$INSTDIR\payload.txt"
  Delete "$INSTDIR\uninstall.exe"
  RMDir "$INSTDIR"
  DeleteRegKey HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\CiukiInstallerFixture"
  DeleteRegValue HKLM "Software\Microsoft\Windows\CurrentVersion\RunOnce" "CiukiInstallerFixture"
FunctionEnd

Section "Uninstall"
  Delete "$DESKTOP\Ciuki Installer Fixture.lnk"
  Delete "$SMPROGRAMS\Ciuki Installer Fixture\Readme.lnk"
  RMDir "$SMPROGRAMS\Ciuki Installer Fixture"
  Delete "$INSTDIR\payload.txt"
  DeleteRegKey HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\CiukiInstallerFixture"
  DeleteRegValue HKLM "Software\Microsoft\Windows\CurrentVersion\RunOnce" "CiukiInstallerFixture"
  Delete /REBOOTOK "$INSTDIR\uninstall.exe"
  RMDir /REBOOTOK "$INSTDIR"
SectionEnd
