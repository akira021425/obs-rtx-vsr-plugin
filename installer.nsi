Name "OBS RTX VSR Plugin"
OutFile "obs-rtx-vsr-1.0.0-windows-x64.exe"
InstallDir "$PROGRAMFILES64\obs-studio"
InstallDirRegKey HKLM "Software\OBS Studio" ""
RequestExecutionLevel admin

Page directory
Page instfiles

Section ""
  SetOutPath $INSTDIR\obs-plugins\64bit
  File "release\RelWithDebInfo\obs-rtx-vsr\bin\64bit\obs-rtx-vsr.dll"
  
  SetOutPath $INSTDIR\data\obs-plugins\obs-rtx-vsr\locale
  File /r "release\RelWithDebInfo\obs-rtx-vsr\data\locale\*.*"
SectionEnd
