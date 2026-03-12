@echo off
setlocal
cd /d %~dp0
set CSC=C:\Windows\Microsoft.NET\Framework64\v4.0.30319\csc.exe
if not exist "%CSC%" set CSC=C:\Windows\Microsoft.NET\Framework\v4.0.30319\csc.exe
if not exist "%CSC%" (
  echo ??? csc.exe
  exit /b 1
)

if not exist bin mkdir bin
if not exist assets mkdir assets

"%CSC%" /nologo /target:winexe /out:bin\EverVance.exe /unsafe- /optimize+ /platform:anycpu /win32icon:assets\EverVance.ico ^
 /reference:System.dll ^
 /reference:System.Core.dll ^
 /reference:System.Drawing.dll ^
 /reference:System.Windows.Forms.dll ^
 /reference:System.Windows.Forms.DataVisualization.dll ^
 src\Program.cs src\MainForm.cs src\Models.cs src\DbcParser.cs src\Transport.cs

if errorlevel 1 (
  echo ????
  exit /b 1
)

echo ????: bin\EverVance.exe
endlocal
