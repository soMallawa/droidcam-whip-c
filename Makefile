# Copyright (C) 2022 DEV47APPS, github.com/dev47apps

BUILD_DIR ?= build
TARGET ?= droidcam-whip

CC ?= gcc
CXX ?= g++
PKG_CONFIG ?= pkg-config
RM ?= rm -f

COMMON_FLAGS += -Wall -Isrc/
CFLAGS += $(COMMON_FLAGS)
CXXFLAGS += -std=c++17 $(COMMON_FLAGS)

PKGS := libavcodec libavformat libavutil libswscale libturbojpeg libusb-1.0 libimobiledevice-1.0 libusbmuxd-2.0
PKG_CFLAGS := $(foreach pkg,$(PKGS),$(shell $(PKG_CONFIG) --cflags $(pkg) 2>/dev/null))
PKG_LIBS := $(foreach pkg,$(PKGS),$(shell $(PKG_CONFIG) --libs $(pkg) 2>/dev/null))

LDFLAGS += -pthread
LDLIBS += $(PKG_LIBS) -lavcodec -lavformat -lavutil -lswscale -lturbojpeg -lusbmuxd-2.0 -limobiledevice-1.0 -ldl

C_SRCS := src/encoder_rtsp.c
CXX_SRCS := \
	src/main.c \
	src/net.cc \
	src/mjpeg_decode.cc \
	src/ffmpeg_decode.cc \
	src/source.cc \
	src/device_discovery.cc \
	src/mdns_discovery.cc \
	src/proxy.cc \
	src/sys/unix/cmd.cc \
	src/sys/unix/util.cc

OBJS := \
	$(BUILD_DIR)/src/main.o \
	$(patsubst %.c,$(BUILD_DIR)/%.o,$(C_SRCS)) \
	$(patsubst %.cc,$(BUILD_DIR)/%.o,$(filter %.cc,$(CXX_SRCS)))

.PHONY: all debug clean

all: $(TARGET)

debug: COMMON_FLAGS += -DDEBUG
debug: clean $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(LDFLAGS) $^ $(LDLIBS) -o $@

$(BUILD_DIR)/src/main.o: src/main.c
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(PKG_CFLAGS) -x c++ -c $< -o $@

$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(PKG_CFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: %.cc
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(PKG_CFLAGS) -c $< -o $@

clean:
	$(RM) -r $(BUILD_DIR) $(TARGET)
