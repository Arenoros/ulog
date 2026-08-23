@echo off
setlocal

set "ULOG_VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%ULOG_VSWHERE%" (
  echo Visual Studio Installer vswhere.exe was not found. Install Visual Studio with the Desktop development with C++ workload. 1>&2
  exit /b 1
)

set "ULOG_VS_INSTALL="
for /f "usebackq delims=" %%I in (`"%ULOG_VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "ULOG_VS_INSTALL=%%I"
if not defined ULOG_VS_INSTALL (
  echo No Visual Studio installation with the MSVC x64 toolchain was found. Install the Desktop development with C++ workload. 1>&2
  exit /b 1
)

call "%ULOG_VS_INSTALL%\Common7\Tools\VsDevCmd.bat" -no_logo -arch=x64
if errorlevel 1 exit /b %errorlevel%

if "%~1"=="" (
  echo Usage: scripts\with-msvc.cmd command [arguments...] 1>&2
  exit /b 2
)

%*
exit /b %errorlevel%
