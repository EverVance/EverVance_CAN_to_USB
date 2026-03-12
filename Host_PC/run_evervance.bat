@echo off
setlocal
cd /d %~dp0
if not exist .\EverVance\bin\EverVance.exe (
  call .\EverVance\build.bat
)
start "EverVance" .\EverVance\bin\EverVance.exe
endlocal
