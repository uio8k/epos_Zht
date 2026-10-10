@ECHO OFF
REM run_headless.bat - Build hd.img and run EPOS (QEMU) headless; auto-quit when the demo finishes.
REM Usage: run_headless.bat [wav [out.wav [marker]]]
REM   (no args) -> QEMU_AUDIO_DRV=dsound (speakers)
REM   wav       -> QEMU_AUDIO_DRV=wav, record to out.wav
REM                (graceful monitor quit is REQUIRED to finalize the wav header)
REM   marker    -> optional completion marker for the watcher (default: piano demo)
REM Logs: epos\userapp\serial.log   Monitor: telnet 127.0.0.1:4444
REM
REM Audio-eval mode: if TEST_MODE / TEST_SIGNAL are set in the environment they are
REM forwarded to make as command-line variables, selecting the test-signal build.
setlocal

set "ROOT=%~dp0.."
for %%I in ("%ROOT%") do set "ROOT=%%~fI"
set "ROOTF=%ROOT:\=/%"
set "PATH=%ROOT%\tools;%ROOT%\Qemu;%ROOT%\msys\2.0\usr\bin;%PATH%"
set "LC_ALL=C"

pushd "%ROOT%\epos" || exit /b 1
set "MAKEARGS="
if defined TEST_MODE   set "MAKEARGS=%MAKEARGS% TEST_MODE=%TEST_MODE%"
if defined TEST_SIGNAL set "MAKEARGS=%MAKEARGS% TEST_SIGNAL=%TEST_SIGNAL%"
make hd.img%MAKEARGS% || (echo [run_headless] build failed & popd & exit /b 1)
popd

set "QEMU_AUDIO_DRV=dsound"
set "QEMU_WAV_PATH="
if /I "%~1"=="wav" set "QEMU_AUDIO_DRV=wav"
if /I "%~1"=="wav" set "QEMU_WAV_PATH=%~2"
if /I "%~1"=="wav" if not defined QEMU_WAV_PATH set "QEMU_WAV_PATH=%ROOT%\out.wav"
echo [run_headless] audio driver: %QEMU_AUDIO_DRV%
if defined QEMU_WAV_PATH echo [run_headless] wav output: %QEMU_WAV_PATH%

REM Clear the old log so the watcher cannot match a stale end-of-demo marker.
if exist "%ROOT%\epos\userapp\serial.log" del /Q "%ROOT%\epos\userapp\serial.log"

REM Background watcher: waits for the end-of-demo marker in serial.log, then sends
REM "quit" through the QEMU monitor so QEMU exits gracefully.
set "MARKER=Demo finished. Check serial.log"
if not "%~3"=="" set "MARKER=%~3"
start "wait_and_quit" /b uv run --no-project "%ROOT%\tools\wait_and_quit.py" "%ROOT%\epos\userapp\serial.log" 4444 600 "%MARKER%"

echo [run_headless] starting QEMU...
qemu-system-i386w.exe -m 32 -boot order=c -vga std ^
  -drive format=raw,file=%ROOTF%/epos/hd.img,index=0,media=disk ^
  -device intel-hda -device hda-duplex ^
  -serial file:%ROOTF%/epos/userapp/serial.log ^
  -display none ^
  -monitor telnet:127.0.0.1:4444,server,nowait

echo [run_headless] QEMU exited.
