@echo off
REM Phase 52: the renderer's patch series.
REM
REM   tools\build_rt64.cmd
REM
REM lib\rt64 is a pinned submodule and stays unmodified (a local change to lib\ is a fork, which is
REM a recorded decision, not an edit). Every change to the renderer is a patch in rt64-patches\,
REM applied here onto a fresh copy of the pinned tree at build-cmake\rt64-src\. CMakeLists.txt
REM builds the renderer from that copy when it exists and from lib\rt64 otherwise.
REM
REM The copy is MIRRORED from the submodule every run (robocopy /MIR, .git excluded), so a patched
REM file is restored to the pinned version before the series is applied again, and a file a patch
REM added is removed if its patch is gone. That is what makes the series the whole change: nothing
REM survives in the copy that is not in lib\rt64 or in a patch. The mirror is cheap after the first
REM run, because robocopy copies only what differs.
REM
REM Prints RT64_SERIES_APPLIED <n> patches, or RT64_SERIES_CLEAN when the folder holds no patch,
REM and refuses with RT64_PATCH_FAILED <name> (exit 1) on the first hunk that does not apply.
REM Refuses with RT64_PIN_MISMATCH when lib\rt64 is not at the SHA the series was written for.

setlocal EnableDelayedExpansion
set "ROOT=%~dp0.."
set "SRC=%ROOT%\lib\rt64"
set "DST=%ROOT%\build-cmake\rt64-src"
set "SERIES=%ROOT%\rt64-patches"
set "PIN=4337374"

for /f %%S in ('git -C "%SRC%" rev-parse --short HEAD') do set "SHA=%%S"
if not "%SHA%"=="%PIN%" (
    echo RT64_PIN_MISMATCH lib\rt64 is at %SHA%, the series is written for %PIN%
    exit /b 1
)

if not exist "%ROOT%\build-cmake" mkdir "%ROOT%\build-cmake"
robocopy "%SRC%" "%DST%" /MIR /XD .git /XF .git /NFL /NDL /NJH /NJS /NP >nul
if errorlevel 8 (
    echo RT64_COPY_FAILED robocopy exit %errorlevel%
    exit /b 1
)

set /a N=0
pushd "%ROOT%"
for %%P in ("%SERIES%\*.patch") do (
    git apply --directory=build-cmake/rt64-src "%%P"
    if errorlevel 1 (
        echo RT64_PATCH_FAILED %%~nxP
        popd
        exit /b 1
    )
    set /a N+=1
)
popd

REM CMake decides at configure time whether the copy exists, so a configured tree is reconfigured
REM here. Cheap: the cache holds every fetched dependency. INSIDE THE MSVC ENVIRONMENT, the same
REM one the tree was configured in: a reconfigure from a bare shell regenerates the resource
REM compiler's rule without the SDK paths that environment carries, and the icon resource then
REM fails to compile with no message at all (2026-09-24, phase 53).
if exist "%ROOT%\build-cmake\CMakeCache.txt" (
    call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
    if errorlevel 1 (
        echo RT64_RECONFIGURE_FAILED vcvars
        exit /b 1
    )
    cmake "%ROOT%\build-cmake" >nul
    if errorlevel 1 (
        echo RT64_RECONFIGURE_FAILED
        exit /b 1
    )
)

if !N!==0 (
    echo RT64_SERIES_CLEAN
) else (
    echo RT64_SERIES_APPLIED !N! patches
)
exit /b 0
