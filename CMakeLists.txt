cmake_minimum_required(VERSION 3.16)
project(droidcam-whip C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

if(WIN32)
    add_compile_definitions(_WIN32_WINNT=0x0601)
    add_executable(droidcam-whip WIN32
        src/main.c
        src/gui_win32.c
        src/net.cc
        src/source.cc
        src/encoder_rtsp.c
        src/mjpeg_decode.cc
        src/ffmpeg_decode.cc
        src/device_discovery.cc
        src/mdns_discovery.cc
        src/proxy.cc
        src/sys/win/cmd.cc
        src/sys/win/util.cc
    )
    target_link_libraries(droidcam-whip
        ws2_32 gdi32 comctl32 pthread
        avcodec avformat avutil swscale turbojpeg
    )
else()
    add_executable(droidcam-whip
        src/main.c
        src/encoder_rtsp.c
        src/net.cc
        src/source.cc
        src/mjpeg_decode.cc
        src/ffmpeg_decode.cc
        src/device_discovery.cc
        src/mdns_discovery.cc
        src/proxy.cc
        src/sys/unix/cmd.cc
        src/sys/unix/util.cc
    )

    find_package(PkgConfig REQUIRED)
    pkg_check_modules(DEPS REQUIRED
        libavcodec
        libavformat
        libavutil
        libswscale
        libturbojpeg
        libusb-1.0
        libimobiledevice-1.0
        libusbmuxd-2.0
    )
    target_link_libraries(droidcam-whip ${DEPS_LIBRARIES} pthread dl)
    target_include_directories(droidcam-whip PRIVATE ${DEPS_INCLUDE_DIRS})
    target_link_directories(droidcam-whip PRIVATE ${DEPS_LIBRARY_DIRS})
    target_compile_options(droidcam-whip PRIVATE ${DEPS_CFLAGS_OTHER})
endif()

target_include_directories(droidcam-whip PRIVATE src)
