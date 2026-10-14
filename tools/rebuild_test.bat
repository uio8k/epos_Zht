@echo off
REM rebuild_test.bat - Clean TEST_MODE stamps, rebuild userapp a.out, and
REM statically verify the binary matches the requested TEST_SIGNAL.
REM
REM Usage:   rebuild_test.bat [tempo|underrun|sine|impulse|step|sequence]
REM          (default: tempo)
REM Exit:    0 = build OK and version check passed
REM          1 = build failed
REM          2 = version check failed (wrong signal left in a.out)
REM
REM ASCII-only on purpose (PowerShell 5.1 decodes scripts without BOM as ANSI).
REM Designed to run as ONE plain line, e.g.:
REM   cmd /c "e:\ComputerScience\epos_Zht\epos_Zht\tools\rebuild_test.bat tempo"

setlocal
REM ROOT = workspace root (holds Qemu/ msys/), REPO = source repo
set "ROOT=%~dp0..\.."
for %%I in ("%ROOT%") do set "ROOT=%%~fI"
set "REPO=%ROOT%\epos_Zht"
set "SIG=%~1"
if "%SIG%"=="" set "SIG=tempo"

REM Same PATH layout as tools\run_headless.bat. Do NOT chain setvars.bat here:
REM its trailing interactive cmd hangs cmd /c chains unless called as
REM "call setvars.bat /ni".
set "PATH=%REPO%\tools;%ROOT%\Qemu;%ROOT%\msys\2.0\usr\bin;%ROOT%\msys\2.0\mingw64\bin;%PATH%"
set "LC_ALL=C"

echo [rebuild_test] signal=%SIG% : cleaning stamps + main.o + a.out
del /Q "%REPO%\userapp\.testmode_*" 2>nul
del /Q "%REPO%\userapp\main.o" "%REPO%\userapp\a.out" 2>nul

pushd "%REPO%\userapp" || (echo [rebuild_test] userapp dir missing & exit /b 1)

make run TEST_MODE=1 TEST_SIGNAL=%SIG%
if errorlevel 1 (
    popd
    echo [rebuild_test] BUILD FAILED
    exit /b 1
)

REM Stamp name is .testmode_<TEST_MODE>_<TEST_SIGNAL>_<SORT_DEMO>
REM (SORT_DEMO suffix empty in default builds), hence the wildcard.
if not exist ".testmode_1_%SIG%*" (
    popd
    echo [rebuild_test] CHECK FAILED: stamp .testmode_1_%SIG%* missing
    exit /b 2
)

REM String markers compiled into a.out (see userapp/main.c run_test_signal):
REM   tempo    -> "notes x" present,  "UNDERRUN" absent
REM   underrun -> "underrun injection" present, "notes x" absent
if /I "%SIG%"=="tempo" (
    findstr /M /C:"notes x" a.out >nul 2>&1
    if errorlevel 1 (popd & echo [rebuild_test] CHECK FAILED: no tempo marker in a.out & exit /b 2)
    findstr /M /C:"UNDERRUN" a.out >nul 2>&1
    if not errorlevel 1 (popd & echo [rebuild_test] CHECK FAILED: UNDERRUN strings present in a.out & exit /b 2)
) else if /I "%SIG%"=="underrun" (
    findstr /M /C:"underrun injection" a.out >nul 2>&1
    if errorlevel 1 (popd & echo [rebuild_test] CHECK FAILED: no underrun marker in a.out & exit /b 2)
    findstr /M /C:"notes x" a.out >nul 2>&1
    if not errorlevel 1 (popd & echo [rebuild_test] CHECK FAILED: tempo strings present in a.out & exit /b 2)
) else (
    echo [rebuild_test] NOTE: no string marker check for signal "%SIG%", stamp check only
)

popd
echo [rebuild_test] OK: a.out is the TEST_SIGNAL=%SIG% build
endlocal
exit /b 0
