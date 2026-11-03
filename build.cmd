@echo off
setlocal
cd /d "%~dp0"
set OUT=out\windows
set ZIG_GLOBAL_CACHE_DIR=%~dp0out\cache\zig
set ZIG_LOCAL_CACHE_DIR=%~dp0out\cache\zig
set RELEASE=https://github.com/signorepesce/Event-Horizon/releases/latest/download
set ENCODE=--no-config --really-quiet --no-audio --of=mp4 --ofopts=use_editlist=0 --ovc=libx265
set TAGS=bf=0,codec_tag=828601960,colorspace=bt709,color_primaries=bt709,color_trc=bt709
set COLOR=out_color_matrix=bt709:out_range=tv,format=yuv420p10le
set NAME=Gargantua-7680x4800-120fps.mp4
set MASTER=media\%NAME%
where zig >nul 2>nul || (echo Event Horizon builds with zig: winget install -e --id zig.zig & exit /b 1)
set ARCH=x86_64
if /i "%PROCESSOR_ARCHITECTURE%"=="ARM64" set ARCH=aarch64
if not exist %OUT%\data mkdir %OUT%\data
zig cc -target %ARCH%-windows-gnu -std=c11 -O2 -municode windows\main.c core\wallpaper.c core\mpv.c windows\app.rc -o %OUT%\event_horizon.exe -Wl,--subsystem,windows -luser32 -lcomctl32 -lshell32 -ldwmapi -lwtsapi32 -lcomdlg32 -lole32 -ladvapi32 -lgdi32 || exit /b 1
del /q %OUT%\event_horizon.pdb 2>nul

set MPV=%OUT%\mpv.exe
if not exist %MPV% (where mpv >nul 2>nul && set MPV=mpv || set MPV=)

if not exist %MASTER% (
    echo Downloading the Gargantua video
    curl -fL --retry 2 --progress-bar -o %MASTER%.part "%RELEASE%/%NAME%" && move /y %MASTER%.part %MASTER% >nul
    del /q %MASTER%.part 2>nul
)

if not exist %MASTER% if defined MPV (
    echo Rendering Gargantua on the CPU: about half a day, only once
    zig cc -target %ARCH%-windows-gnu -std=c11 -O2 render\gargantua.c -o out\gargantua.exe || exit /b 1
    del /q out\render.failed 2>nul
    (out\gargantua.exe 7680 4800 24 || echo.>out\render.failed) | "%MPV%" %ENCODE% --ovcopts=crf=16,preset=medium,g=240,%TAGS% --vf=lavfi=[scale=%COLOR%] --demuxer=rawvideo --demuxer-rawvideo-w=7680 --demuxer-rawvideo-h=4800 --demuxer-rawvideo-mp-format=rgb48le --demuxer-rawvideo-fps=120 --o=%MASTER%.part -
    if exist out\render.failed (del /q out\render.failed %MASTER%.part 2>nul) else (move /y %MASTER%.part %MASTER% >nul)
)

del /q %OUT%\Gargantua-*.mp4 2>nul
if exist %MASTER% copy /y %MASTER% %OUT%\ >nul
if not exist %OUT%\%NAME% echo The Gargantua video is missing: the app will start, and you can choose your own video or image.
if not defined MPV echo To play and to render Gargantua, put mpv.exe in %OUT% (https://mpv.io/installation) and run build.cmd again.
echo Ready: %OUT%\event_horizon.exe
