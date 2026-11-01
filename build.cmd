@echo off
setlocal
cd /d "%~dp0"
set OUT=out\windows
set RELEASE=https://github.com/signorepesce/Event-Horizon/releases/latest/download
set ENCODE=--no-config --really-quiet --no-audio --of=mp4 --ofopts=use_editlist=0 --ovc=libx264
set TAGS=bf=0,colorspace=bt709,color_primaries=bt709,color_trc=bt709
set COLOR=out_color_matrix=bt709:out_range=tv,format=yuv420p
set TOP=media\Gargantua-3840x2400-60fps.mp4
where zig >nul 2>nul || (echo Event Horizon builds with zig: winget install -e --id zig.zig & exit /b 1)
set ARCH=x86_64
if /i "%PROCESSOR_ARCHITECTURE%"=="ARM64" set ARCH=aarch64
if not exist %OUT% mkdir %OUT%
zig cc -target %ARCH%-windows-gnu -std=c11 -O2 -municode windows\main.c core\wallpaper.c core\mpv.c windows\app.rc -o %OUT%\event_horizon.exe -Wl,--subsystem,windows -luser32 -lcomctl32 -lshell32 -ldwmapi -lwtsapi32 -lcomdlg32 -lole32 -ladvapi32 -lgdi32 || exit /b 1
del /q %OUT%\event_horizon.pdb 2>nul

set MPV=%OUT%\mpv.exe
if not exist %MPV% (where mpv >nul 2>nul && set MPV=mpv || set MPV=)

for %%s in (1920x1200 3072x1920 3840x2400) do for %%r in (30 60) do (
    if not exist media\Gargantua-%%s-%%rfps.mp4 (
        curl -fsL --retry 2 -o media\Gargantua-%%s-%%rfps.mp4.part "%RELEASE%/Gargantua-%%s-%%rfps.mp4" && move /y media\Gargantua-%%s-%%rfps.mp4.part media\Gargantua-%%s-%%rfps.mp4 >nul
        del /q media\Gargantua-%%s-%%rfps.mp4.part 2>nul
    )
)

if not exist %TOP% if defined MPV (
    echo Rendering Gargantua on the CPU: about half an hour, only once
    zig cc -target %ARCH%-windows-gnu -std=c11 -O2 render\gargantua.c -o out\gargantua.exe || exit /b 1
    del /q out\render.failed 2>nul
    (out\gargantua.exe 3840 2400 24 || echo.>out\render.failed) | "%MPV%" %ENCODE% --ovcopts=crf=16,preset=slow,g=120,%TAGS% --vf=lavfi=[scale=%COLOR%] --demuxer=rawvideo --demuxer-rawvideo-w=3840 --demuxer-rawvideo-h=2400 --demuxer-rawvideo-mp-format=rgb24 --demuxer-rawvideo-fps=60 --o=%TOP%.part -
    if exist out\render.failed (del /q out\render.failed %TOP%.part 2>nul) else (move /y %TOP%.part %TOP% >nul)
)

if exist %TOP% if defined MPV (
    for %%s in (1920x1200 3072x1920) do for /f "tokens=1,2 delims=x" %%w in ("%%s") do (
        if not exist media\Gargantua-%%s-60fps.mp4 "%MPV%" %ENCODE% %TOP% --ovcopts=crf=18,preset=medium,g=120,%TAGS% --vf=lavfi=[scale=%%w:%%x:flags=lanczos:%COLOR%] --o=media\Gargantua-%%s-60fps.mp4.part && move /y media\Gargantua-%%s-60fps.mp4.part media\Gargantua-%%s-60fps.mp4 >nul
    )
    for %%s in (1920x1200 3072x1920 3840x2400) do for /f "tokens=1,2 delims=x" %%w in ("%%s") do (
        if not exist media\Gargantua-%%s-30fps.mp4 "%MPV%" %ENCODE% %TOP% --ovcopts=crf=18,preset=medium,g=60,%TAGS% --vf=lavfi=[framestep=2,scale=%%w:%%x:flags=lanczos:%COLOR%] --o=media\Gargantua-%%s-30fps.mp4.part && move /y media\Gargantua-%%s-30fps.mp4.part media\Gargantua-%%s-30fps.mp4 >nul
    )
)

for %%s in (1920x1200 3072x1920 3840x2400) do for %%r in (30 60) do (
    if exist media\Gargantua-%%s-%%rfps.mp4 copy /y media\Gargantua-%%s-%%rfps.mp4 %OUT%\ >nul
)
if not exist %OUT%\Gargantua-1920x1200-30fps.mp4 echo The Gargantua videos are missing: the app will start, and you can choose your own video or image.
if not defined MPV echo To play and to render Gargantua, put mpv.exe in %OUT% (https://mpv.io/installation) and run build.cmd again.
echo Ready: %OUT%\event_horizon.exe
