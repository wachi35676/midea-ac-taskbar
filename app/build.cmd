@echo off
rem Builds app\build\ACTaskbar.exe with Visual Studio 2022 (C++ desktop workload). No other dependencies.
setlocal
set "ROOT=%~dp0"
set "NATIVE=%ROOT%..\native\src\midea"
for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath`) do set "VS=%%i"
if not defined VS (echo Visual Studio not found & exit /b 1)
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
if not exist "%ROOT%build" mkdir "%ROOT%build"
rc /nologo /fo "%ROOT%build\app.res" "%ROOT%res\app.rc" || exit /b 1
cl /nologo /std:c++20 /EHsc /W4 /WX /permissive- /O1 /MT /utf-8 /DUNICODE /D_UNICODE /DNOMINMAX /DWIN32_LEAN_AND_MEAN ^
   /D_CRT_SECURE_NO_WARNINGS /D_WIN32_WINNT=0x0A00 /Fo"%ROOT%build\\" /Fe"%ROOT%build\ACTaskbar.exe" ^
   "%ROOT%src\main.cpp" "%ROOT%src\worker.cpp" "%ROOT%src\widget.cpp" "%ROOT%src\flyout.cpp" "%ROOT%src\gfx.cpp" "%ROOT%src\art.cpp" "%ROOT%src\schedule.cpp" "%ROOT%src\settings.cpp" "%ROOT%src\demo.cpp" "%ROOT%src\backdrop.cpp" ^
   "%NATIVE%\crypto.cpp" "%NATIVE%\lan.cpp" "%NATIVE%\discover.cpp" "%NATIVE%\ac_protocol.cpp" ^
   "%NATIVE%\ac_device.cpp" "%NATIVE%\ac_controller.cpp" "%NATIVE%\creds.cpp" "%ROOT%build\app.res" ^
   /link /SUBSYSTEM:WINDOWS user32.lib gdi32.lib advapi32.lib shell32.lib ole32.lib oleaut32.lib comctl32.lib ^
   d2d1.lib dwrite.lib dwmapi.lib shcore.lib bcrypt.lib ws2_32.lib || exit /b 1
echo Built %ROOT%build\ACTaskbar.exe
