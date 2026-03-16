@echo off
REM Build script for MGR:R D3D9 FFP Proxy DLL
REM Requires MSVC (Visual Studio Build Tools) - x86 target

REM Find vcvarsall.bat
for /f "tokens=*" %%i in ('"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath 2^>nul') do set VSDIR=%%i

if not defined VSDIR (
    echo ERROR: Visual Studio not found. Install VS Build Tools.
    exit /b 1
)

set VCVARSALL=%VSDIR%\VC\Auxiliary\Build\vcvarsall.bat
if not exist "%VCVARSALL%" (
    echo ERROR: vcvarsall.bat not found at %VCVARSALL%
    exit /b 1
)

echo Setting up x86 build environment...
call "%VCVARSALL%" x86 >nul 2>&1

echo Compiling d3d9_main.c...
cl.exe /nologo /O1 /GS- /W3 /Zl /c /D "WIN32" /D "NDEBUG" d3d9_main.c
if errorlevel 1 goto :error

echo Compiling d3d9_wrapper.c...
cl.exe /nologo /O1 /GS- /W3 /Zl /c /D "WIN32" /D "NDEBUG" d3d9_wrapper.c
if errorlevel 1 goto :error

echo Compiling d3d9_device.c...
cl.exe /nologo /O1 /Oi /GS- /W3 /Zl /c /D "WIN32" /D "NDEBUG" d3d9_device.c
if errorlevel 1 goto :error

echo Linking d3d9.dll...
link.exe /nologo /DLL /NODEFAULTLIB /ENTRY:_DllMainCRTStartup@12 /DEF:d3d9.def /OUT:d3d9.dll d3d9_main.obj d3d9_wrapper.obj d3d9_device.obj kernel32.lib
if errorlevel 1 goto :error

echo.
echo === Build successful: d3d9.dll ===
echo.
echo Deploy: copy d3d9.dll to your MGR:R game directory
echo   copy d3d9.dll "d:\GOG Games\METAL GEAR RISING REVENGEANCE\"
echo.

REM Clean intermediates
del *.obj *.lib *.exp 2>nul

exit /b 0

:error
echo.
echo === BUILD FAILED ===
exit /b 1
