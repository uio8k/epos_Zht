@ECHO OFF
REM run_headless.bat - Build hd.img and run EPOS (QEMU) headless; auto-quit when the demo finishes.
REM Usage: run_headless.bat [wav [out.wav [marker]]]
REM   (no args) -> QEMU_AUDIO_DRV=dsound (speakers)
REM   wav       -> QEMU_AUDIO_DRV=wav, record to out.wav
REM                (graceful monitor quit is REQUIRED to finalize the wav header)
REM   marker    -> optional completion marker for the watcher (default: piano demo)
REM Logs: epos_Zht\userapp\serial.log   Monitor: telnet 127.0.0.1:4444
REM
REM Audio-eval mode: if TEST_MODE / TEST_SIGNAL are set in the environment they are
REM forwarded to make as command-line variables, selecting the test-signal build.
setlocal

REM 本脚本位于 <工作区根>/epos_Zht/tools/：
REM   ROOT = 工作区根（含 Qemu/ msys/ setvars.bat）
REM   REPO = 源码仓库 <工作区根>/epos_Zht
set "ROOT=%~dp0..\.."
for %%I in ("%ROOT%") do set "ROOT=%%~fI"
set "REPO=%ROOT%\epos_Zht"
set "ROOTF=%ROOT:\=/%"
REM mingw64\bin 提供 mtools（mcopy，写入 hd.img 所需），usr\bin 提供 make/gcc
set "PATH=%REPO%\tools;%ROOT%\Qemu;%ROOT%\msys\2.0\usr\bin;%ROOT%\msys\2.0\mingw64\bin;%PATH%"
set "LC_ALL=C"

pushd "%REPO%" || exit /b 1
set "MAKEARGS="
if defined TEST_MODE   set "MAKEARGS=%MAKEARGS% TEST_MODE=%TEST_MODE%"
if defined TEST_SIGNAL set "MAKEARGS=%MAKEARGS% TEST_SIGNAL=%TEST_SIGNAL%"
make hd.img%MAKEARGS% || (echo [run_headless] build failed & popd & exit /b 1)
popd

set "QEMU_AUDIO_DRV=dsound"
set "QEMU_WAV_PATH="
if /I "%~1"=="wav" set "QEMU_AUDIO_DRV=wav"
if /I "%~1"=="wav" set "QEMU_WAV_PATH=%~2"
if /I "%~1"=="wav" if not defined QEMU_WAV_PATH set "QEMU_WAV_PATH=%REPO%\out.wav"
echo [run_headless] audio driver: %QEMU_AUDIO_DRV%
if defined QEMU_WAV_PATH echo [run_headless] wav output: %QEMU_WAV_PATH%

REM Clear the old log so the watcher cannot match a stale end-of-demo marker.
if exist "%REPO%\userapp\serial.log" del /Q "%REPO%\userapp\serial.log"

REM Background watcher: waits for the end-of-demo marker in serial.log, then sends
REM "quit" through the QEMU monitor so QEMU exits gracefully.
set "MARKER=Demo finished. Check serial.log"
if not "%~3"=="" set "MARKER=%~3"
REM 第 4 参数：watcher 超时秒数（默认 600）。宁短勿长：QEMU 的 -serial file:
REM 输出带缓冲，外部进程通常读不到实时 marker（实测），靠超时兜底发 quit
REM 优雅退出（QEMU 退出时 flush 串口，日志与 wav 头才能完整落盘）。
set "WATCH=600"
if not "%~4"=="" set "WATCH=%~4"
start "wait_and_quit" /b uv run --no-project "%REPO%\tools\wait_and_quit.py" "%REPO%\userapp\serial.log" 4444 %WATCH% "%MARKER%"

REM -m 48：内核堆 12MB + 用户堆 32MB，内存不足会导致启动失败
echo [run_headless] starting QEMU...
qemu-system-i386w.exe -m 48 -boot order=c -vga std ^
  -drive format=raw,file=%ROOTF%/epos_Zht/hd.img,index=0,media=disk ^
  -device intel-hda -device hda-duplex ^
  -serial file:%ROOTF%/epos_Zht/userapp/serial.log ^
  -display none ^
  -monitor telnet:127.0.0.1:4444,server,nowait

echo [run_headless] QEMU exited.
