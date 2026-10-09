@echo off
REM Phase 31: prove the PIPELINE, not the build.
REM
REM Every generated artifact is deleted and regenerated from the committed inputs: the symbol
REM tables, the configs, the patch sources. If this fails, something was hand edited in a
REM generated directory, which is the failure this exists to catch and which is otherwise
REM invisible until somebody tries to rebuild in six months.
REM
REM TWO THINGS THIS GOT WRONG ONCE, BOTH WORTH KEEPING IN MIND:
REM
REM 1. It deleted patches.elf but not the patch OBJECT FILES, so make relinked from stale objects
REM    and reported success. The patch sources were never recompiled and the run passed anyway.
REM    A reproducibility check that reuses yesterday's output is not a reproducibility check.
REM
REM 2. It ran `make` directly on Windows. The patch build runs in WSL, because no Windows clang
REM    carries a MIPS backend: neither the Visual Studio one nor the official llvm.org release
REM    registers mips at all. tools\build_patches.cmd is what crosses over.

call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 (echo VCVARS FAILED & exit /b 1)

REM make is not on PATH by default. It arrives with the toolchain as the winget package
REM ezwinports.make and lands here, and vcvars does not add it.
set "PATH=C:\Program Files\WinGet\Links;%PATH%"

REM The project root is wherever this script lives, one level up. It was a hard coded path once
REM and the project moved; a stale path here rebuilds the wrong tree or nothing at all.
cd /d "%~dp0.."

REM THE SAVE LIVES INSIDE build-cmake, so deleting it destroys the player's file. Backed up first
REM and restored at the end, outside the repository: a save is game derived and is never committed.
set SAVEBAK=%TEMP%\oot-save-backup
if exist "build-cmake\saves" (
  if not exist "%SAVEBAK%" mkdir "%SAVEBAK%"
  xcopy /y /q /s "build-cmake\saves\*" "%SAVEBAK%\" >nul
  echo SAVE_BACKED_UP
)

REM THE A AGAINST B CAPTURES LIVE THERE TOO (phase 51 onward, build-cmake\harness\abshots): the
REM baselines every lighting phase compares against. Kept the same way, outside the repository,
REM because a frame of the game is never committed and a baseline that has to be retaken after
REM every rebuild is not a baseline.
set SHOTBAK=%TEMP%\oot-abshots-backup
if exist "build-cmake\harness\abshots" (
  if not exist "%SHOTBAK%" mkdir "%SHOTBAK%"
  xcopy /y /q /s /e "build-cmake\harness\abshots\*" "%SHOTBAK%\" >nul
  echo ABSHOTS_BACKED_UP
)

REM AND THE PORTABLE SETTINGS AND CONTROLS (build-cmake\settings.txt, controls.txt): a build tree
REM runs portable, so the harness plays on these, and every capture depends on them. Lost once,
REM on 2026-09-24: the rebuild after phase 52 dropped the file, the game fell back to the struct
REM defaults (native resolution), and every Off proof of phase 53 read DIFFERENT against a
REM baseline taken at three times native until the reason was found.
set CFGBAK=%TEMP%\oot-config-backup
if not exist "%CFGBAK%" mkdir "%CFGBAK%"
if exist "build-cmake\settings.txt" copy /y "build-cmake\settings.txt" "%CFGBAK%\" >nul
if exist "build-cmake\controls.txt" copy /y "build-cmake\controls.txt" "%CFGBAK%\" >nul
echo CONFIG_BACKED_UP

echo === deleting every generated artifact ===
if exist RecompiledFuncs rmdir /s /q RecompiledFuncs
if exist RecompiledPatches rmdir /s /q RecompiledPatches
if exist build-cmake rmdir /s /q build-cmake
if exist src\rsp del /q src\rsp\*.cpp 2>nul
if exist patches\patches.elf del /q patches\patches.elf
del /q patches\*.o patches\*.d 2>nul
del /q patches\required\*.o patches\required\*.d 2>nul
del /q patches\fixes\*.o patches\fixes\*.d 2>nul
echo DELETED_OK

echo === main recompiler ===
.\build\bin\N64Recomp.exe config\ntsc-1.0.toml
if errorlevel 1 (echo RECOMPILER FAILED & exit /b 1)
echo RECOMPILER_OK

echo === rsp microcode ===
.\build\bin\RSPRecomp.exe config\aspMain.ntsc-1.0.toml
if errorlevel 1 (echo ASPMAIN FAILED & exit /b 1)
.\build\bin\RSPRecomp.exe config\njpgdspMain.ntsc-1.0.toml
if errorlevel 1 (echo NJPGDSP FAILED & exit /b 1)
echo RSP_OK

echo === patches, which build in WSL ===
call tools\build_patches.cmd
if errorlevel 1 (echo PATCH BUILD FAILED & exit /b 1)
.\build\bin\N64Recomp.exe config\patches.toml
if errorlevel 1 (echo PATCH RECOMPILE FAILED & exit /b 1)
echo PATCHES_RECOMPILED_OK

echo === the renderer's patch series (phase 52) ===
REM build-cmake was deleted above, so the copy is made from nothing and the series reapplied in
REM full. This is the proof that the series is the whole change to the renderer: a hand edit in
REM build-cmake\rt64-src is gone here, exactly as one in a generated directory is.
call tools\build_rt64.cmd
if errorlevel 1 (echo RT64 SERIES FAILED & exit /b 1)

echo === configure and build ===
cmake -S . -B build-cmake -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl
if errorlevel 1 (echo CONFIGURE FAILED & exit /b 1)
cmake --build build-cmake --target OoTRecompiled
if errorlevel 1 (echo BUILD FAILED & exit /b 1)
echo BUILD_OK

echo === the rom has to be put back beside the new binary ===
copy /y "Legend of Zelda, The - Ocarina of Time (USA).z64" "build-cmake\" >nul
if errorlevel 1 (echo ROM COPY FAILED & exit /b 1)
echo ROM_OK

echo === restoring the save and the captures ===
if exist "%SAVEBAK%" (
  if not exist "build-cmake\saves" mkdir "build-cmake\saves"
  xcopy /y /q /s "%SAVEBAK%\*" "build-cmake\saves\" >nul
  echo SAVE_RESTORED
)
if exist "%SHOTBAK%" (
  if not exist "build-cmake\harness\abshots" mkdir "build-cmake\harness\abshots"
  xcopy /y /q /s /e "%SHOTBAK%\*" "build-cmake\harness\abshots\" >nul
  echo ABSHOTS_RESTORED
)
if exist "%CFGBAK%\settings.txt" copy /y "%CFGBAK%\settings.txt" "build-cmake\" >nul
if exist "%CFGBAK%\controls.txt" copy /y "%CFGBAK%\controls.txt" "build-cmake\" >nul
echo CONFIG_RESTORED

echo PHASE31_COMPLETE
