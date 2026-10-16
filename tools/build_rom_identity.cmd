@echo off
REM Build and run tools/rom_identity.cpp, which prints the XXH3 hash and internal name of a ROM.
REM One off: its output is pasted into src/game/game_init.cpp.
REM
REM   tools\build_rom_identity.cmd "<rom path>"

call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 (echo VCVARS FAILED & exit /b 1)

set ROOT=%~dp0..
set OUT=%ROOT%\build\rom_identity.exe

clang-cl /nologo /O2 /EHsc /std:c++20 ^
    /I"%ROOT%\lib\N64ModernRuntime\thirdparty\xxHash" ^
    "%ROOT%\tools\rom_identity.cpp" ^
    /Fe"%OUT%" /Fo"%ROOT%\build\rom_identity.obj"
if errorlevel 1 (echo COMPILE FAILED & exit /b 1)

"%OUT%" %1
