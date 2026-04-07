@echo off
setlocal

:: ── ForceBumpTerrain ASI build script ──────────────────────────────────
:: Compiles patch.c into ForceBumpTerrain.asi using MSVC (x64).
::
:: Usage:
::   build.bat                           Auto-detect Visual Studio
::   build.bat "path\to\vcvarsall.bat"   Use explicit vcvarsall path
:: ───────────────────────────────────────────────────────────────────────

set "NAME=ForceBumpTerrain"
set "ARCH=x64"
set "SRC=%~dp0patch.c"
set "OBJ=%~dp0patch.obj"
set "OUT=%~dp0%NAME%.asi"

:: ── Resolve vcvarsall.bat ──────────────────────────────────────────────
if not "%~1"=="" (
    set "VCVARSALL=%~1"
    goto :check_vcvarsall
)

:: Try vswhere auto-detection
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo [ERROR] vswhere not found. Pass vcvarsall.bat path as argument.
    echo         build.bat "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat"
    exit /b 1
)

for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -property installationPath`) do set "VSINSTALL=%%i"
if not defined VSINSTALL (
    echo [ERROR] No Visual Studio installation found via vswhere.
    exit /b 1
)

set "VCVARSALL=%VSINSTALL%\VC\Auxiliary\Build\vcvarsall.bat"

:check_vcvarsall
if not exist "%VCVARSALL%" (
    echo [ERROR] vcvarsall.bat not found at:
    echo         %VCVARSALL%
    exit /b 1
)

:: ── Build ──────────────────────────────────────────────────────────────
echo [build] Activating MSVC for %ARCH%...
call "%VCVARSALL%" %ARCH% >nul 2>&1

echo [build] Compiling %SRC%...
cl.exe /nologo /O1 /GS- /W3 /Zl /D "WIN32" /D "NDEBUG" /c /Fo"%OBJ%" "%SRC%"
if errorlevel 1 (
    echo [ERROR] Compilation failed.
    exit /b 1
)

echo [build] Linking %NAME%.asi...
link.exe /nologo /DLL /NODEFAULTLIB /ENTRY:DllMain /OUT:"%OUT%" "%OBJ%" kernel32.lib
if errorlevel 1 (
    echo [ERROR] Link failed.
    exit /b 1
)

:: ── Cleanup intermediates ──────────────────────────────────────────────
if exist "%OBJ%" del "%OBJ%"
if exist "%~dp0%NAME%.lib" del "%~dp0%NAME%.lib"
if exist "%~dp0%NAME%.exp" del "%~dp0%NAME%.exp"

:: ── Done ───────────────────────────────────────────────────────────────
for %%F in ("%OUT%") do set "SIZE=%%~zF"
echo [build] OK  %OUT%  (%SIZE% bytes)

endlocal
