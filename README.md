# droidcam-whip-c

Push DroidCam phone video directly to a remote face-swap server via RTSP — zero OBS, zero Python, zero overhead.

This is a stripped-down fork of the [droidcam-obs-plugin](https://github.com/dev47apps/droidcam-obs-plugin). All OBS/Qt dependencies removed. Replaced with a lightweight FFmpeg RTSP encoder and an optional Win32 GUI.

## Architecture

```
Phone (DroidCam app, TCP :4747)
    │  HTTP GET /v5/video/avc/1280x720/...
    │  Custom binary stream (12B header + encoded video)
    ▼
droidcam-whip-c
    │  H.264/MJPEG decode (TurboJPEG / libavcodec)
    │  BGR → YUV conversion (sws_scale)
    │  NVENC H.264 encode (p1/ll/zerolatency)
    │  RTSP push (libavformat)
    ▼
MediaMTX :8554/cam_in
    │  RTSP → WebRTC WHIP/WHEP
    ▼
deep-live-cam-remote (vast.ai GPU)
    │  Face swap (inswapper_128.onnx)
    │  WHEP output back to you
    ▼
OBS Browser Source / WHEP player
```

## Features

- **Zero OBS dependency** — phone feed goes directly to the server
- **In-process FFmpeg** (libavcodec/libavformat) — no CLI subprocess, minimal latency
- **NVENC hardware encode** — p1 preset, low-latency tune, CBR bitrate
- **H.264 passthrough** — if phone sends AVC, no re-encode needed (`--no-copy-video` to force re-encode)
- **Exponential backoff reconnection** — handles WiFi drops gracefully
- **Win32 GUI** (Windows only) — live preview, FPS counter, settings dialog, INI persistence
- **Headless console mode** — works on Linux and Windows without GUI

## Quick Start

### Windows (with GUI)

```powershell
# 1. Install MSYS2: https://www.msys2.org/
# 2. Clone & build
git clone https://github.com/soMallawa/droidcam-whip-c
cd droidcam-whip-c
.\build.ps1

# 3. Launch GUI
.\droidcam-whip.exe --gui
```

The GUI window lets you enter the phone IP, RTSP server URL, and video settings. Click Connect to start streaming.

### Windows / Linux (headless)

```bash
# Build (Linux requires: libavcodec-dev libavformat-dev libavutil-dev libswscale-dev libturbojpeg-dev)
make

# Run
./droidcam-whip 192.168.1.100 rtsp://your-server:8554/cam_in
```

### Options

```
droidcam-whip [--gui] phone-ip [rtsp-url] [options]

  phone-ip           Phone IP shown in DroidCam app
  rtsp-url           RTSP endpoint (default: rtsp://127.0.0.1:8554/cam_in)
  --gui              Launch Windows GUI (ignored on Linux)
  --port PORT        DroidCam TCP port (default: 4747)
  --width WIDTH      Video width (default: 1280)
  --height HEIGHT    Video height (default: 720)
  --fps FPS          Frame rate (default: 25)
  --format avc|hevc|jpg  Video format (default: avc)
  --no-hw            Disable hardware decode
  --no-copy-video    Re-encode AVC instead of passthrough
```

## Phone Setup

1. Install [DroidCam](https://www.dev47apps.com/) on your phone
2. Connect phone and PC to the same WiFi
3. Open DroidCam app — note the IP address shown
4. Run droidcam-whip with that IP

## Server Setup

See [deep-live-cam-remote](https://github.com/soMallawa/deep-live-cam-remote) for the GPU face-swap server.

```bash
# On vast.ai GPU instance:
git clone https://github.com/soMallawa/deep-live-cam-remote
cd deep-live-cam-remote
cp /path/to/source-face.jpg source.jpg
docker compose up --build
```

## Building

### Linux

```bash
# Install dependencies
sudo apt install libavcodec-dev libavformat-dev libavutil-dev \
                 libswscale-dev libturbojpeg-dev libusb-1.0-dev \
                 libimobiledevice-dev libusbmuxd-dev

# Build
make
```

### Windows (MSYS2/MinGW-w64)

```powershell
# One-click:
.\build.ps1

# Or manual:
# In MSYS2 UCRT64 terminal:
pacman -S mingw-w64-x86_64-{ffmpeg,libturbojpeg,cmake,toolchain}
mkdir build && cd build
cmake .. -G "MinGW Makefiles"
mingw32-make
```

## How It Works

The original droidcam-obs-plugin already had a complete, optimized pipeline: TCP networking, mDNS discovery, binary protocol parsing, TurboJPEG/FFmpeg hardware decode. The only OBS-specific code was one function call: `obs_source_output_video2()`.

This fork replaces that single call with an in-process FFmpeg RTSP encoder. Everything else — networking, decode, connection management — is the original, battle-tested C++ code.

### Files kept from original:
- `src/net.cc` — TCP connect/send/recv (WinSock2 + POSIX)
- `src/mjpeg_decode.cc` — TurboJPEG MJPEG decoder
- `src/ffmpeg_decode.cc` — libavcodec H.264/H.265 decoder
- `src/source.cc` — connection management, binary protocol parser
- `src/device_discovery.cc` — mDNS, ADB, iOS device discovery

### Files added:
- `src/encoder_rtsp.c` — NVENC/libx264 encoder + RTSP push (263 lines)
- `src/main.c` — CLI entry point with `--gui` support (257 lines)
- `src/gui_win32.c` — Win32 preview window (489 lines)
- `CMakeLists.txt` — cross-platform build
- `build.ps1` — one-click Windows build script

### Files removed:
- `src/plugin.cc` — OBS plugin loader
- `src/ui/AddDevice.*` — Qt GUI
- `src/test/` — OBS test stubs

## License

GPLv2 — same as the original droidcam-obs-plugin.
