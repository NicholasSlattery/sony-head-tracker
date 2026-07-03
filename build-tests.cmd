@echo off
rem Build and run the pure-core unit tests (run-tests.exe). These compile only the
rem hardware-independent sources (maths, descriptor decode, orientation filter,
rem protocol serialisation) plus the tests -- no Windows, no headset required.
setlocal

where cl >nul 2>nul && goto build

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
  echo Could not find Visual Studio. Install "Desktop development with C++",
  echo or open a "x64 Native Tools Command Prompt for VS" and run this script there.
  exit /b 1
)
set "PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;%PATH%"
set "VSPATH="
for /f "usebackq tokens=*" %%i in (`vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
if not defined VSPATH (
  echo Visual Studio with the C++ toolset was not found.
  exit /b 1
)
call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul

:build
if not exist build md build
cl /nologo /std:c++latest /EHsc /permissive- /utf-8 /W4 /I include /I tests /Fobuild\ ^
   src\math.cpp src\hid_descriptor.cpp src\orientation.cpp src\protocol.cpp src\app_config.cpp src\diagnostics.cpp ^
   tests\test_main.cpp tests\math_tests.cpp tests\descriptor_tests.cpp tests\orientation_tests.cpp tests\protocol_tests.cpp tests\config_tests.cpp tests\diagnostics_tests.cpp ^
   /Fe:run-tests.exe
if not %errorlevel%==0 (
  echo.
  echo Test build failed.
  exit /b 1
)
echo.
"%~dp0run-tests.exe"
if not %errorlevel%==0 (
  echo.
  echo Tests FAILED.
  exit /b 1
)
endlocal
