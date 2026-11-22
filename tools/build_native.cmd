@echo off
REM Build the game (the native side) inside the MSVC environment it was configured in.
REM
REM   tools\build_native.cmd            build the OoTRecompiled target
REM
REM WHY THIS WRAPPER EXISTS. The build is configured with clang-cl from the Visual Studio 2022
REM Build Tools, and clang-cl finds the MSVC headers and libraries through the environment that
REM vcvars64.bat sets up. Run `cmake --build` from a bare shell and clang picks the NEWEST MSVC
REM toolset installed on the machine instead, which here is a Visual Studio 18 one whose standard
REM library refuses any clang older than 20:
REM
REM   yvals_core.h: error STL1000: Unexpected compiler version, expected Clang 20 or newer.
REM
REM That reads like a broken standard library and is really a missing environment. It only shows
REM when a C++ file recompiles; a build that merely relinks (a patch change) succeeds from anywhere,
REM which is how the bare-shell habit survives until it does not.
REM
REM The exe's timestamp is checked rather than the wrapper's own exit code, because the link fails
REM with "permission denied" while the game is running and that has been misread as success before.

setlocal
set "ROOT=%~dp0.."
set "EXE=%ROOT%\build-cmake\OoTRecompiled.exe"

call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 (echo VCVARS FAILED & exit /b 1)

for %%F in ("%EXE%") do set "BEFORE=%%~tF"

cmake --build "%ROOT%\build-cmake" --target OoTRecompiled
if errorlevel 1 (echo NATIVE BUILD FAILED & exit /b 1)

for %%F in ("%EXE%") do set "AFTER=%%~tF"
echo exe before: %BEFORE%
echo exe after:  %AFTER%
if "%BEFORE%"=="%AFTER%" (
    echo NATIVE BUILD DID NOT PRODUCE A NEW EXE ^(nothing to rebuild, or the link failed^)
    exit /b 2
)
echo NATIVE_OK
