@echo off
rem Build xm5-headtracker.exe from the single source file.
rem Run this from any normal Command Prompt -- it finds Visual Studio's C++ tools
rem automatically. (Or run it from a "x64 Native Tools Command Prompt for VS".)
setlocal

where cl >nul 2>nul && goto build

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
  echo Could not find Visual Studio. Install "Desktop development with C++",
  echo or open a "x64 Native Tools Command Prompt for VS" and run this script there.
  exit /b 1
)

rem Put the Installer dir on PATH so vswhere can be called without a quoted path
rem (a leading quote confuses "for /f").
set "PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;%PATH%"
set "VSPATH="
for /f "usebackq tokens=*" %%i in (`vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
if not defined VSPATH (
  echo Visual Studio with the C++ toolset was not found.
  exit /b 1
)
call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul

:build
rc /nologo app.rc
if not %errorlevel%==0 (
  echo.
  echo Resource compile failed.
  exit /b 1
)
cl /nologo /std:c++latest /EHsc /permissive- /utf-8 /O2 /W4 /DUNICODE /D_UNICODE xm5_head_tracker.cpp app.res /Fe:xm5-headtracker.exe
if %errorlevel%==0 (
  del /q xm5_head_tracker.obj app.res >nul 2>nul
  echo.
  echo Built xm5-headtracker.exe
) else (
  echo.
  echo Build failed.
  exit /b 1
)
endlocal
