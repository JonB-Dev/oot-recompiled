@echo off
REM Build the MIPS patch ELF, which happens in WSL because no Windows clang has a MIPS backend.
REM
REM   tools\build_patches.cmd          build
REM   tools\build_patches.cmd clean    clean
REM
REM The project lives on the Windows filesystem and is reached from WSL through /mnt/c. That is
REM slower than the Linux filesystem but correct: the patch sources, the linker scripts and the
REM decomp headers all live here, and keeping one copy is worth more than the speed.

set TARGET=%1
if "%TARGET%"=="" set TARGET=all

REM The folder is found from where this script sits, never written in. It was written in until
REM 2026-10-07, when the project moved (to Nextcloud-Personal, as oot-recompiled) and this line went
REM on cd-ing into a folder that no longer existed. wsl.exe --cd takes the Windows path and starts
REM there, spaces and all.
wsl.exe --cd "%~dp0..\patches" -e bash -lc "make %TARGET%"
if errorlevel 1 (echo PATCH BUILD FAILED & exit /b 1)

echo PATCHES_OK

REM The recompile is part of the build, not a separate step to remember. It refuses (exit code 1
REM and nothing written) when a patch reaches a symbol the game does not have, and a native build
REM run after a refused recompile relinks the PREVIOUS patch output and reports success, which
REM happened once (phase 39, a fused sincosf). So the refusal has to stop the chain here.
if "%TARGET%"=="clean" exit /b 0
cd /d "%~dp0.."
.\build\bin\N64Recomp.exe config\patches.toml
if errorlevel 1 (echo PATCH RECOMPILE FAILED, RecompiledPatches is unchanged & exit /b 1)

echo RECOMPILED_OK
