@echo off
setlocal
cd /d %~dp0
if not exist .\bin\EverVance.exe (
  call .\build.bat
)
start "" .\bin\EverVance.exe
endlocal
