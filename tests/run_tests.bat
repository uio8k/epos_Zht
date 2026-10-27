@echo off
rem Host-side L1 unit tests + test-plan self check (ASCII only).
rem Why a .bat instead of building inside make: the repo's bundled MSYS
rem (e:/ComputerScience/epos_Zht/msys/2.0) is a header-less, stripped distro.
rem A libc-capable gcc (C:\msys64\mingw64) launched from that MSYS sh dies
rem silently (exit 1, no diagnostics); launched from cmd it works.
setlocal
rem 必须把 PATH 换成 mingw64 自己的目录：从 make（本仓 MSYS 的 sh）继承的 PATH 里
rem 有 msys/2.0/usr/bin，其 msys-2.0.dll 会让 mingw64 的 gcc 静默退出（exit 1、无输出）。
set "PATH=C:\msys64\mingw64\bin;%SystemRoot%\system32;%SystemRoot%"
set "REPO=%~dp0.."
set "CC=C:\msys64\mingw64\bin\gcc.exe"
set "PY=python.exe"
set "RC=0"
cd /d "%~dp0"

if not exist build mkdir build
copy /Y "%REPO%\kernel\hda_capture.c"   build\hda_capture_under_test.c >nul
copy /Y "%REPO%\kernel\hda_internal.h"  build\hda_internal.h >nul
copy /Y "%REPO%\include\audio_capture.h" build\audio_capture.h >nul
copy /Y host\kernel.h                   build\kernel.h >nul
copy /Y host\cpu.h                      build\cpu.h >nul
copy /Y host\machdep.h                  build\machdep.h >nul

rem ---------- 1) codec 配置层（U-L1-01..03 所在文件） ----------
"%CC%" -std=gnu11 -Wall -Wextra -g -O1 -I. -Iunity -Ibuild -I"%REPO%\kernel" ^
       -o build\test_capture_probe_fresh.exe test_capture_probe.c unity\unity.c
if errorlevel 1 (echo [tests] BUILD FAILED: test_capture_probe & exit /b 1)

build\test_capture_probe_fresh.exe
if errorlevel 1 set "RC=1"

rem ---------- 2) 指标用例（U-L1-06..10，期望值来自 fixtures\vectors.c） ----------
"%CC%" -std=gnu11 -Wall -Wextra -g -O1 -I. -Iunity -Ifixtures -I"%REPO%\userapp" ^
       -o build\test_metrics_fresh.exe test_metrics.c fixtures\vectors.c ^
          "%REPO%\userapp\metrics.c" unity\unity.c
if errorlevel 1 (echo [tests] BUILD FAILED: test_metrics & exit /b 1)

build\test_metrics_fresh.exe
if errorlevel 1 set "RC=1"

rem ---------- 3) 测试计划自洽性 ----------
rem docs/test-plan-audio.md 已于 2026-10-28 删除，校验脚本随之停用；
rem 脚本本体留在 tests\tools\check_plan.py，计划若恢复（同名同结构）即可接回。

if not "%RC%"=="0" (echo [tests] FAILED & exit /b 1)
echo [tests] ALL PASS
exit /b 0
