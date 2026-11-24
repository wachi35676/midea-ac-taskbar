@echo off
rem Builds native\build\acctl.exe with Visual Studio 2022 (C++ desktop workload). No other dependencies.
setlocal
set "ROOT=%~dp0"
for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath`) do set "VS=%%i"
if not defined VS (echo Visual Studio not found & exit /b 1)
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
if not exist "%ROOT%build" mkdir "%ROOT%build"
cl /nologo /std:c++17 /EHsc /W4 /WX /permissive- /O1 /MT /utf-8 /DNOMINMAX /DWIN32_LEAN_AND_MEAN /D_CRT_SECURE_NO_WARNINGS /D_WIN32_WINNT=0x0A00 ^
   /Fo"%ROOT%build\\" /Fe"%ROOT%build\acctl.exe" ^
   "%ROOT%tools\acctl.cpp" "%ROOT%src\midea\crypto.cpp" "%ROOT%src\midea\lan.cpp" "%ROOT%src\midea\discover.cpp" ^
   "%ROOT%src\midea\ac_protocol.cpp" "%ROOT%src\midea\ac_device.cpp" "%ROOT%src\midea\ac_controller.cpp" "%ROOT%src\midea\creds.cpp" ^
   /link bcrypt.lib ws2_32.lib || exit /b 1
echo Built %ROOT%build\acctl.exe
