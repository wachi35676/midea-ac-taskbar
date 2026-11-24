@echo off
rem Builds and runs the art preview tool: renders contact sheets of app\src\art.cpp into app\build\preview\*.png.
setlocal
set "ROOT=%~dp0"
set "OUT=%ROOT%build\preview"
for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath`) do set "VS=%%i"
if not defined VS (echo Visual Studio not found & exit /b 1)
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
if not exist "%OUT%\obj" mkdir "%OUT%\obj"
cl /nologo /std:c++20 /EHsc /W4 /WX /permissive- /O1 /MT /utf-8 /DUNICODE /D_UNICODE /DNOMINMAX /DWIN32_LEAN_AND_MEAN ^
   /D_CRT_SECURE_NO_WARNINGS /D_WIN32_WINNT=0x0A00 /I"%ROOT%src" /Fo"%OUT%\obj\\" /Fe"%OUT%\obj\art_preview.exe" ^
   "%ROOT%tools\art_preview.cpp" "%ROOT%src\art.cpp" ^
   /link /SUBSYSTEM:CONSOLE user32.lib d2d1.lib dwrite.lib windowscodecs.lib ole32.lib || exit /b 1
"%OUT%\obj\art_preview.exe" "%OUT%" || exit /b 1
echo Rendered previews into %OUT%
