SHELL := /bin/bash

include app.mk

ROOT := $(CURDIR)
DEPS ?= $(ROOT)/deps
TOOLCHAIN_ROOT := $(if $(PALM_TOOLCHAIN_ROOT),$(PALM_TOOLCHAIN_ROOT),$(DEPS)/palm-toolchain)
PREFIX := $(if $(PALM_TOOLCHAIN_PREFIX),$(PALM_TOOLCHAIN_PREFIX),$(TOOLCHAIN_ROOT)/.toolchain/prefix)
PALM_TLS := $(DEPS)/palm-tls
HTTP_COMPONENT := $(PALM_TLS)/components/http
SDK := $(PREFIX)/palmdev/sdk/include
CC := $(PREFIX)/bin/m68k-none-elf-gcc
OBJCOPY := $(PREFIX)/bin/m68k-none-elf-objcopy
OBJDUMP := $(PREFIX)/bin/m68k-none-elf-objdump
SIZE := $(PREFIX)/bin/m68k-none-elf-size
READELF := $(PREFIX)/bin/m68k-none-elf-readelf
# Fails if a function starts at an odd address: the 68000 cannot run it
# there, and the Palm resets.
CHECK_EVEN = $(READELF) -sW $(1) | awk '$$4 == "FUNC" && $$2 ~ /[13579bdfBDF]$$/ \
	{ print "odd function address: " $$8; odd = 1 } END { exit odd }'
PILRC := $(PREFIX)/bin/pilrc
BUILD_PRC := $(PREFIX)/bin/build-prc
MAGICK ?= $(shell command -v magick 2>/dev/null || command -v convert 2>/dev/null || printf magick)
HOST_CC ?= cc

PROFILE ?= debug
BUILD_DIR := $(ROOT)/build/$(PROFILE)
HOST_DIR := $(ROOT)/build/host
ELF := $(BUILD_DIR)/code0001.68k.elf
CODE := $(BUILD_DIR)/code0001.bin
PRC := $(BUILD_DIR)/$(APP_ID).prc
BASE_RO := $(BUILD_DIR)/$(APP_ID).ro
RESOURCE_RCP := $(BUILD_DIR)/app.rcp
MAP := $(BUILD_DIR)/$(APP_ID).map

CORE_SOURCES := $(wildcard src/core/*.c)
# 68000 calls reach only +/-32 KB, and the code is larger than that, so
# tools/m68k-veneers.py gives each object long-call veneers for the
# functions it calls in other files. Data references stay within a file.
LINK_ORDER := palm/startup palm/arm_module core/mp4_read palm/playback \
	core/mp4_merge core/mp4_frag palm/store palm/main core/text core/json_scan core/yt_api \
	palm/net_job core/mp4_stream palm/stream_feed palm/udp_log
OBJECTS := $(patsubst %,$(BUILD_DIR)/%.o,$(LINK_ORDER)) $(BUILD_DIR)/http.o
FAR_DIR := $(BUILD_DIR)/far
FAR_OBJECTS := $(patsubst $(BUILD_DIR)/%,$(FAR_DIR)/app/%,$(OBJECTS))
DEPFILES := $(OBJECTS:.o=.d)
# Decoder benchmark app: the 68K side plus the native ARM module.
BENCH_ID := PalmViewBench
BENCH_NAME := PalmView Bench
BENCH_CREATOR := PVbn
BENCH_LINK_ORDER := palm/startup palm/arm_module palm/store bench/main core/text \
	core/mp4_read
BENCH_OBJECTS := $(patsubst %,$(BUILD_DIR)/%.o,$(BENCH_LINK_ORDER))
BENCH_FAR_OBJECTS := $(patsubst $(BUILD_DIR)/%,$(FAR_DIR)/bench/%,$(BENCH_OBJECTS))
BENCH_DIR := $(BUILD_DIR)/bench
BENCH_ELF := $(BENCH_DIR)/code0001.68k.elf
BENCH_PRC := $(BENCH_DIR)/$(BENCH_ID).prc

# Native ARM module (see tools/armlink.py). Built with Arch's/Debian's
# arm-none-eabi-gcc and newlib; FFmpeg and musl come from tools/build-arm-deps.sh.
ARM_CC ?= arm-none-eabi-gcc
ARM_DIR := $(ROOT)/build/arm
ARM_CFLAGS := -marm -march=armv5t -mtune=xscale -mfloat-abi=soft -fno-short-enums \
	-fPIC -msingle-pic-base -mno-pic-data-is-text-relative -mlong-calls \
	-ffunction-sections -fdata-sections -fno-common -fno-merge-constants
FFMPEG_ARM := $(DEPS)/ffmpeg-build-arm
FFMPEG_HOST := $(DEPS)/ffmpeg-build-host
ARM_INCLUDES := -Isrc/arm -I$(DEPS)/ffmpeg -I$(FFMPEG_ARM)
ARM_OBJECTS := $(ARM_DIR)/entry.o $(ARM_DIR)/dispatch.o $(ARM_DIR)/decoder.o \
	$(ARM_DIR)/sound.o $(ARM_DIR)/libc.o $(ARM_DIR)/ffstubs.o
ARM_LIB_FILES := $(FFMPEG_ARM)/libavcodec/libavcodec.a $(FFMPEG_ARM)/libavutil/libavutil.a \
	$(DEPS)/musl-build-arm/libm.a
ARM_LDFLAGS := -nostdlib -Wl,--gc-sections -Wl,--emit-relocs -Wl,--no-enum-size-warning \
	-Wl,--no-warn-rwx-segments -Wl,-u,ArmDispatch
ARM_RCP := $(ARM_DIR)/res/arm.rcp


SDK_INCLUDES := -isystem$(SDK) -isystem$(SDK)/Core -isystem$(SDK)/Core/Hardware \
	-isystem$(SDK)/Core/System -isystem$(SDK)/Core/UI -isystem$(SDK)/Dynamic \
	-isystem$(SDK)/Libraries -isystem$(SDK)/Libraries/PalmOSGlue
ABI_FLAGS := -Wno-multichar -m68000 -mno-align-int -mpcrel -fpic -fshort-enums -mshort
WARN_FLAGS := -Wall -Wextra -Wsign-compare -Wno-unused-parameter
# -fno-merge-constants keeps each file's strings next to its code; merged
# strings can land out of PC-relative reach.
COMMON_FLAGS := $(ABI_FLAGS) $(WARN_FLAGS) -std=gnu17 -MMD -MP -ffunction-sections \
	-fdata-sections -fno-merge-constants -DPALMOS -DAPP_CREATOR="'$(APP_CREATOR)'" -Isrc/core -Isrc/palm -Isrc/arm \
	-I$(PALM_TLS)/library/include -I$(HTTP_COMPONENT)/include $(SDK_INCLUDES)
# Debug builds optimize for size too: -O1 code no longer fits in 64 KB.
PROFILE_FLAGS := $(if $(filter release,$(PROFILE)),-Os,-Os -g)
LDFLAGS := $(ABI_FLAGS) $(PROFILE_FLAGS) -nostartfiles -Wl,--gc-sections \
	-Wl,-T,$(ROOT)/src/palm/linker.lkr -Wl,-Map,$(MAP)

HOST_FLAGS := -std=c99 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror -O1 -g \
	-fsanitize=address,undefined -Ihost/shim -Isrc/core -I$(HTTP_COMPONENT)/include
PVIEW := $(HOST_DIR)/pview
JSON_DUMP := $(HOST_DIR)/json_dump
UNIT_TESTS := $(HOST_DIR)/unit_tests
STREAM_TEST := $(HOST_DIR)/stream_test
FEED_TEST := $(HOST_DIR)/feed_test
RGB_TEST := $(HOST_DIR)/rgb_test

.PHONY: all debug release bench tls dist test test-arm test-playback host arm clean
.DELETE_ON_ERROR:

all: debug

debug:
	@$(MAKE) --no-print-directory PROFILE=debug $(ROOT)/build/debug/$(APP_ID).prc
release:
	@$(MAKE) --no-print-directory PROFILE=release $(ROOT)/build/release/$(APP_ID).prc
bench:
	@$(MAKE) --no-print-directory PROFILE=release $(ROOT)/build/release/bench/$(BENCH_ID).prc

# PalmTLS library PRCs (PalmTLSArm.prc is the one for the Palm TX).
tls:
	@$(MAKE) -C $(PALM_TLS) check

# Everything to install on the device.
dist: release bench
	@test -f $(PALM_TLS)/library/build/palm/PalmTLSArm.prc || $(MAKE) tls
	mkdir -p dist
	cp $(ROOT)/build/release/$(APP_ID).prc $(PALM_TLS)/library/build/palm/PalmTLSArm.prc \
		$(ROOT)/build/release/bench/$(BENCH_ID).prc dist/
	@ls -l dist

$(BUILD_DIR) $(BUILD_DIR)/core $(BUILD_DIR)/palm $(BUILD_DIR)/bench $(HOST_DIR) $(ARM_DIR):
	@mkdir -p $@

$(RESOURCE_RCP): src/palm/app.rcp $(ARM_RCP) app.mk | $(BUILD_DIR)
	sed 's/@APP_VERSION@/$(APP_VERSION)/g' $< > $@
	sed 's#"\(arm[a-z]*[0-9]*\.bin\)"#"$(ARM_DIR)/res/\1"#' $(ARM_RCP) >> $@

$(BUILD_DIR)/%.o: src/%.c app.mk | $(BUILD_DIR)/core $(BUILD_DIR)/palm $(BUILD_DIR)/bench
	$(CC) $(COMMON_FLAGS) $(PROFILE_FLAGS) -c $< -o $@

$(BUILD_DIR)/http.o: $(HTTP_COMPONENT)/src/http.c | $(BUILD_DIR)
	$(CC) $(COMMON_FLAGS) $(PROFILE_FLAGS) -c $< -o $@

# The linker script drops .data and .bss: 68K Palm code resources here have
# no globals. Fail loudly instead of shipping code that reads garbage.
$(BUILD_DIR)/globals.ok: $(OBJECTS) $(BENCH_OBJECTS)
	@for object in $^; do \
		if $(OBJDUMP) -h $$object | awk '$$2 ~ /^\.(data|bss)/ && $$3 != "00000000" { found = 1 } END { exit !found }'; then \
			echo "$$object has .data or .bss; use stack or heap memory instead." >&2; exit 1; \
		fi; \
	done
	@touch $@

VENEERS := python3 -I tools/m68k-veneers.py $(PREFIX)/bin/m68k-none-elf- \
	--lib "$$($(CC) $(ABI_FLAGS) -print-libgcc-file-name)" \
	--lib "$$($(CC) $(ABI_FLAGS) -print-file-name=libc.a)"

$(FAR_DIR)/app.stamp: $(OBJECTS) tools/m68k-veneers.py
	$(VENEERS) $(FAR_DIR)/app $(BUILD_DIR) $(OBJECTS)
	@touch $@

$(FAR_DIR)/bench.stamp: $(BENCH_OBJECTS) tools/m68k-veneers.py
	$(VENEERS) $(FAR_DIR)/bench $(BUILD_DIR) $(BENCH_OBJECTS)
	@touch $@

$(ELF): $(FAR_DIR)/app.stamp $(BUILD_DIR)/globals.ok src/palm/linker.lkr
	$(CC) -o $@.stage1 $(LDFLAGS) -Wl,--emit-relocs $(FAR_OBJECTS)
	node $(TOOLCHAIN_ROOT)/tools/generate-m68k-relocs.mjs $@.stage1 \
		$(BUILD_DIR)/reloc_offsets.s $(PREFIX)/bin/m68k-none-elf-readelf
	$(CC) $(ABI_FLAGS) -c $(BUILD_DIR)/reloc_offsets.s -o $(BUILD_DIR)/reloc_offsets.o
	$(CC) -o $@ $(LDFLAGS) -Wl,--emit-relocs $(FAR_OBJECTS) $(BUILD_DIR)/reloc_offsets.o
	rm -f $@.stage1
	@$(call CHECK_EVEN,$@)

$(CODE): $(ELF)
	$(OBJCOPY) -O binary $< $@ -j .vec -j .text

# Launcher icons are 32x22, and 64x44 for high-density screens. Palm bitmaps
# have one transparent color and no partial transparency. Pixels that are
# mostly transparent stay so; the rest are blended with a light background
# to soften the edge, which shows as a faint halo on dark wallpapers.
ICONS := $(BUILD_DIR)/icon-color.bmp $(BUILD_DIR)/icon-color-hd.bmp
ICON_EDGE := \( -clone 0 -alpha off -fill '\#ff00ff' -colorize 100 \) \
	\( -clone 0 -background '\#dce6f0' -alpha remove -alpha off \) \
	\( -clone 0 -alpha extract -threshold 15% \) \
	-delete 0 -composite -colors 256 -compress none

$(BUILD_DIR)/icon-color.bmp: src/palm/icon.svg | $(BUILD_DIR)
	$(MAGICK) -background none -density 96 RSVG:$< $(ICON_EDGE) BMP3:$@
$(BUILD_DIR)/icon-color-hd.bmp: src/palm/icon.svg | $(BUILD_DIR)
	$(MAGICK) -background none -density 192 RSVG:$< $(ICON_EDGE) BMP3:$@

$(PRC): $(CODE) $(ICONS) \
		$(RESOURCE_RCP) src/palm/resource.h certs/google.pem app.mk
	cd $(BUILD_DIR) && $(PILRC) -ro -o $(BASE_RO) -creator $(APP_CREATOR) \
		-type $(APP_TYPE) -name '$(APP_NAME)' -I $(ROOT)/src/palm $(RESOURCE_RCP)
	$(BUILD_PRC) --no-check -o $(PRC) -n '$(APP_NAME)' -c $(APP_CREATOR) \
		-t $(APP_TYPE) -v $(APP_DB_VERSION) $(BASE_RO)
	python3 -I tools/align-prc.py $(PRC)
	node $(TOOLCHAIN_ROOT)/tools/validate-prc.mjs $(PRC) '$(APP_NAME)' \
		$(APP_TYPE) $(APP_CREATOR)
	$(SIZE) $(ELF)


# Bench app

$(BENCH_DIR)/app.rcp: src/bench/bench.rcp $(ARM_RCP) app.mk | $(BUILD_DIR)/bench
	sed 's/@APP_VERSION@/$(APP_VERSION)/g' $< > $@
	sed 's#"\(arm[a-z]*[0-9]*\.bin\)"#"$(ARM_DIR)/res/\1"#' $(ARM_RCP) >> $@

$(BENCH_ELF): $(FAR_DIR)/bench.stamp $(BUILD_DIR)/globals.ok src/palm/linker.lkr
	$(CC) -o $@.stage1 $(ABI_FLAGS) $(PROFILE_FLAGS) -nostartfiles -Wl,--gc-sections \
		-Wl,-T,$(ROOT)/src/palm/linker.lkr -Wl,--emit-relocs $(BENCH_FAR_OBJECTS)
	node $(TOOLCHAIN_ROOT)/tools/generate-m68k-relocs.mjs $@.stage1 \
		$(BENCH_DIR)/reloc_offsets.s $(PREFIX)/bin/m68k-none-elf-readelf
	$(CC) $(ABI_FLAGS) -c $(BENCH_DIR)/reloc_offsets.s -o $(BENCH_DIR)/reloc_offsets.o
	$(CC) -o $@ $(ABI_FLAGS) $(PROFILE_FLAGS) -nostartfiles -Wl,--gc-sections \
		-Wl,-T,$(ROOT)/src/palm/linker.lkr -Wl,-Map,$(BENCH_DIR)/bench.map \
		-Wl,--emit-relocs $(BENCH_FAR_OBJECTS) $(BENCH_DIR)/reloc_offsets.o
	@$(call CHECK_EVEN,$@)
	rm -f $@.stage1

$(BENCH_DIR)/code0001.bin: $(BENCH_ELF)
	$(OBJCOPY) -O binary $< $@ -j .vec -j .text

$(BENCH_PRC): $(BENCH_DIR)/code0001.bin $(ICONS) \
		$(BENCH_DIR)/app.rcp src/bench/resource.h
	cp $(ICONS) $(BENCH_DIR)/
	cd $(BENCH_DIR) && $(PILRC) -q -ro -o $(BENCH_DIR)/$(BENCH_ID).ro -creator $(BENCH_CREATOR) \
		-type $(APP_TYPE) -name '$(BENCH_NAME)' -I $(ROOT)/src/bench app.rcp
	$(BUILD_PRC) --no-check -o $@ -n '$(BENCH_NAME)' -c $(BENCH_CREATOR) \
		-t $(APP_TYPE) -v 1 $(BENCH_DIR)/$(BENCH_ID).ro
	python3 -I tools/align-prc.py $@
	node $(TOOLCHAIN_ROOT)/tools/validate-prc.mjs $@ '$(BENCH_NAME)' $(APP_TYPE) $(BENCH_CREATOR)
	$(SIZE) $(BENCH_ELF)

# Native ARM module: link once to measure, segment, link again, package.

arm: $(ARM_RCP)

$(ARM_DIR)/%.o: src/arm/%.c $(wildcard src/arm/*.h) | $(ARM_DIR)
	$(ARM_CC) $(ARM_CFLAGS) -O2 -std=gnu11 -Wall -Wextra $(ARM_INCLUDES) -c $< -o $@

$(ARM_DIR)/entry.o: src/arm/entry.S | $(ARM_DIR)
	$(ARM_CC) $(ARM_CFLAGS) -c $< -o $@

$(ARM_DIR)/pass1.ld: tools/armlink.py | $(ARM_DIR)
	python3 -I tools/armlink.py first-pass $@

$(ARM_DIR)/pass1.map: $(ARM_DIR)/pass1.ld $(ARM_OBJECTS) $(ARM_LIB_FILES)
	$(ARM_CC) $(ARM_CFLAGS) $(ARM_LDFLAGS) -Wl,-T,$< -Wl,-Map,$@ $(ARM_OBJECTS) \
		$(ARM_LIB_FILES) -lc -lgcc -o $(ARM_DIR)/pass1.elf

$(ARM_DIR)/pass2.ld: $(ARM_DIR)/pass1.map tools/armlink.py
	python3 -I tools/armlink.py layout $< $@

$(ARM_DIR)/pass2.elf: $(ARM_DIR)/pass2.ld $(ARM_OBJECTS) $(ARM_LIB_FILES)
	$(ARM_CC) $(ARM_CFLAGS) $(ARM_LDFLAGS) -Wl,-T,$< -Wl,-Map,$(ARM_DIR)/pass2.map \
		$(ARM_OBJECTS) $(ARM_LIB_FILES) -lc -lgcc -o $@
	@! $(ARM_CC:gcc=objdump) -d $@ | grep -qE "\s(ldrd|strd)\s" || \
		{ echo "ldrd/strd found: code in resources is only 4-byte aligned" >&2; exit 1; }

$(ARM_RCP): $(ARM_DIR)/pass2.elf tools/armlink.py
	rm -rf $(ARM_DIR)/res
	python3 -I tools/armlink.py package $< $(ARM_DIR)/res

# Host tools and tests

host: $(PVIEW)

$(PVIEW): host/pview.c $(CORE_SOURCES) $(HTTP_COMPONENT)/src/http.c \
		$(wildcard src/core/*.h) host/shim/PalmOS.h | $(HOST_DIR)
	$(HOST_CC) $(HOST_FLAGS) host/pview.c $(CORE_SOURCES) \
		$(HTTP_COMPONENT)/src/http.c -lssl -lcrypto -o $@

$(JSON_DUMP): host/tests/json_dump.c src/core/json_scan.c src/core/json_scan.h \
		host/shim/PalmOS.h | $(HOST_DIR)
	$(HOST_CC) $(HOST_FLAGS) host/tests/json_dump.c src/core/json_scan.c -o $@

$(UNIT_TESTS): host/tests/unit_tests.c $(CORE_SOURCES) $(wildcard src/core/*.h) \
		host/shim/PalmOS.h | $(HOST_DIR)
	$(HOST_CC) $(HOST_FLAGS) host/tests/unit_tests.c $(CORE_SOURCES) -o $@

$(STREAM_TEST): host/tests/stream_test.c $(CORE_SOURCES) $(wildcard src/core/*.h) \
		host/shim/PalmOS.h | $(HOST_DIR)
	$(HOST_CC) $(HOST_FLAGS) host/tests/stream_test.c $(CORE_SOURCES) -o $@

$(FEED_TEST): host/tests/feed_test.c src/palm/stream_feed.c src/palm/stream_feed.h \
		src/palm/net_job.h $(CORE_SOURCES) $(wildcard src/core/*.h) \
		host/shim/PalmOS.h | $(HOST_DIR)
	$(HOST_CC) $(HOST_FLAGS) -Isrc/palm host/tests/feed_test.c $(CORE_SOURCES) -o $@

# Offline tests. JSON samples in testdata/ (not committed) are compared
# with Python's parser when present. Downloaded streams in testdata/media/
# are read as they would arrive over the network, through small rings.
test: $(JSON_DUMP) $(UNIT_TESTS) $(PVIEW) $(STREAM_TEST) $(FEED_TEST) $(RGB_TEST)
	$(UNIT_TESTS)
	$(RGB_TEST)
	@for video in $(wildcard testdata/media/*.video.mp4); do \
		id=$$(basename $$video .video.mp4); audio=testdata/media/$$id.audio.mp4; \
		$(PVIEW) merge $$video $$audio $(HOST_DIR)/$$id.merged.mp4 > /dev/null 2>&1 || exit 1; \
		for part in $$video $$audio; do \
			for ring in 64 24; do \
				$(STREAM_TEST) $$part $(HOST_DIR)/$$id.merged.mp4 $$ring $$ring || exit 1; \
			done; \
		done; \
		for seed in 1 2 3; do \
			$(FEED_TEST) $$video $$audio 191 320 $$seed || exit 1; \
			$(FEED_TEST) $$video $$audio 48 48 $$seed || exit 1; \
		done; \
	done
	@for file in host/tests/edge.json $(wildcard testdata/*.json); do \
		for seed in 1 7; do \
			$(JSON_DUMP) $$file $$seed > $(HOST_DIR)/c.txt || exit 1; \
			python3 -I host/tests/json_reference.py $$file > $(HOST_DIR)/p.txt; \
			cmp -s $(HOST_DIR)/c.txt $(HOST_DIR)/p.txt || { echo "JSON mismatch: $$file"; exit 1; }; \
		done; \
		echo "json ok: $$file"; \
	done


BENCH_HOST := $(HOST_DIR)/bench

$(RGB_TEST): host/tests/rgb_test.c src/arm/decoder.c src/arm/decoder.h \
		$(FFMPEG_HOST)/libavcodec/libavcodec.a $(FFMPEG_HOST)/libavutil/libavutil.a | $(HOST_DIR)
	$(HOST_CC) $(HOST_FLAGS) -Isrc/arm -I$(DEPS)/ffmpeg -I$(FFMPEG_HOST) \
		host/tests/rgb_test.c $(FFMPEG_HOST)/libavcodec/libavcodec.a \
		$(FFMPEG_HOST)/libavutil/libavutil.a -lm -o $@

$(BENCH_HOST): host/bench.c src/core/mp4_read.c src/arm/decoder.c $(wildcard src/core/*.h) \
		src/arm/decoder.h $(FFMPEG_HOST)/libavcodec/libavcodec.a | $(HOST_DIR)
	$(HOST_CC) -std=gnu11 -Wall -Wextra -O2 -g -Ihost/shim -Isrc/core -Isrc/arm \
		-I$(DEPS)/ffmpeg -I$(FFMPEG_HOST) host/bench.c src/core/mp4_read.c src/arm/decoder.c \
		$(FFMPEG_HOST)/libavcodec/libavcodec.a $(FFMPEG_HOST)/libavutil/libavutil.a -lm -o $@

PLAYBACK_SIM := $(HOST_DIR)/playback_sim

# playback.c includes its headers with quotes, which would find the device
# versions next to it, so a copy is built beside the host stand-ins.
$(PLAYBACK_SIM): host/tests/playback_sim.c src/palm/playback.c src/palm/playback.h \
		src/core/mp4_read.c src/core/mp4_stream.c src/core/mp4_frag.c \
		src/arm/decoder.c src/arm/sound.c src/arm/sound.h \
		$(wildcard host/tests/playsim/*.h) $(FFMPEG_HOST)/libavcodec/libavcodec.a | $(HOST_DIR)
	mkdir -p $(HOST_DIR)/playsim
	cp src/palm/playback.c src/palm/playback.h $(HOST_DIR)/playsim/
	$(HOST_CC) -std=gnu11 -Wall -Wextra -Wno-pointer-to-int-cast -O2 -g \
		-Ihost/tests/playsim -I$(HOST_DIR)/playsim \
		-Ihost/shim -Isrc/core -Isrc/arm -I$(DEPS)/ffmpeg -I$(FFMPEG_HOST) \
		host/tests/playback_sim.c $(HOST_DIR)/playsim/playback.c src/core/mp4_read.c \
		src/core/mp4_stream.c src/core/mp4_frag.c \
		src/arm/decoder.c src/arm/sound.c $(FFMPEG_HOST)/libavcodec/libavcodec.a \
		$(FFMPEG_HOST)/libavutil/libavutil.a -lm -o $@

# Plays each downloaded test video against a simulated Palm TX clock, then
# streams each pair of DASH files at 40 KB/s with a 15 s outage at 20 s,
# and again seeking forward, back, and near the end.
test-playback: $(PLAYBACK_SIM)
	@for file in testdata/media/*.mp4; do \
		case "$$file" in *.video.mp4|*.audio.mp4) continue;; esac; \
		echo "== $$file"; \
		out=$$($(PLAYBACK_SIM) "$$file" 1.0 5) || { echo "$$out"; exit 1; }; \
		echo "$$out" | tail -3; \
	done
	@for video in testdata/media/*.video.mp4; do \
		audio=$${video%.video.mp4}.audio.mp4; \
		echo "== streaming $$video"; \
		out=$$($(PLAYBACK_SIM) --stream $$video $$audio 40 20 15) || \
			{ echo "$$out"; exit 1; }; \
		echo "$$out" | tail -4; \
		echo "== seeking in $$video"; \
		out=$$($(PLAYBACK_SIM) --stream $$video $$audio 40 \
			--seek 10 120 --seek 30 5 --seek 50 3600) || { echo "$$out"; exit 1; }; \
		echo "$$out" | grep -E "^seek|again|sound:"; \
	done

# Runs the packaged ARM module in a PXA270 emulator and checks it decodes
# each downloaded test video exactly like the host build. The second run
# sends the sound through the ring and the stream callback.
test-arm: $(ARM_RCP) $(BENCH_HOST)
	@for file in testdata/media/*.mp4; do \
		case "$$file" in *.video.mp4|*.audio.mp4) continue;; esac; \
		for skip in 0 2; do \
			want=$$($(BENCH_HOST) "$$file" 4 $$skip | grep -E '^(video|audio):' | \
				grep -o 'checksum [0-9a-f]*' | tr '\n' ' '); \
			got=$$($(DEPS)/venv/bin/python host/tests/arm_emulate.py $(ARM_DIR)/res "$$file" \
				--seconds 4 --skip $$skip $$( [ $$skip = 2 ] && echo --queue 3 --sound ) | \
				grep -E '^(video|audio):' | grep -o 'checksum [0-9a-f]*' | tr '\n' ' '); \
			if [ "$$want" = "$$got" ]; then echo "arm ok (skip $$skip): $$file"; \
			else echo "ARM MISMATCH (skip $$skip): $$file: host $$want, arm $$got"; exit 1; fi; \
		done; \
	done

clean:
	rm -rf build dist

-include $(DEPFILES)
