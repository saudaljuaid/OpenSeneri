# SPDX-License-Identifier: Zlib
# SDL 2.32.10 common sources plus Phipia's public-ABI platform backends.

SDL2_SOURCES := \
	$(wildcard vendor/sdl2/src/*.c) \
	$(wildcard vendor/sdl2/src/atomic/*.c) \
	$(wildcard vendor/sdl2/src/audio/*.c) \
	$(wildcard vendor/sdl2/src/audio/phipia/*.c) \
	$(wildcard vendor/sdl2/src/cpuinfo/*.c) \
	$(wildcard vendor/sdl2/src/events/*.c) \
	$(wildcard vendor/sdl2/src/file/*.c) \
	$(wildcard vendor/sdl2/src/haptic/*.c) \
	$(wildcard vendor/sdl2/src/haptic/dummy/*.c) \
	$(wildcard vendor/sdl2/src/hidapi/*.c) \
	$(wildcard vendor/sdl2/src/joystick/*.c) \
	$(wildcard vendor/sdl2/src/joystick/dummy/*.c) \
	$(wildcard vendor/sdl2/src/loadso/dummy/*.c) \
	$(wildcard vendor/sdl2/src/power/*.c) \
	$(wildcard vendor/sdl2/src/filesystem/phipia/*.c) \
	$(wildcard vendor/sdl2/src/locale/*.c) \
	$(wildcard vendor/sdl2/src/locale/dummy/*.c) \
	$(wildcard vendor/sdl2/src/misc/*.c) \
	$(wildcard vendor/sdl2/src/misc/dummy/*.c) \
	$(wildcard vendor/sdl2/src/render/*.c) \
	$(wildcard vendor/sdl2/src/render/software/*.c) \
	$(wildcard vendor/sdl2/src/sensor/*.c) \
	$(wildcard vendor/sdl2/src/sensor/dummy/*.c) \
	$(wildcard vendor/sdl2/src/stdlib/*.c) \
	$(wildcard vendor/sdl2/src/libm/*.c) \
	$(wildcard vendor/sdl2/src/thread/*.c) \
	$(wildcard vendor/sdl2/src/thread/phipia/*.c) \
	$(wildcard vendor/sdl2/src/timer/*.c) \
	$(wildcard vendor/sdl2/src/timer/phipia/*.c) \
	$(wildcard vendor/sdl2/src/video/*.c) \
	$(wildcard vendor/sdl2/src/video/yuv2rgb/*.c) \
	$(wildcard vendor/sdl2/src/video/phipia/*.c)
