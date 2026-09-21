# Builds weather.wasm, the TRACE module -- the picture half of the door.
# Needs wasi-sdk (34 or later); override with WASI_SDK=... if yours lives elsewhere.
MAKEFLAGS += -r
.SUFFIXES:

WASI_SDK ?= $(HOME)/tools/wasi-sdk-34.0-x86_64-linux
CC        = $(WASI_SDK)/bin/clang

# Prefer the canonical header in the TERMinator tree; fall back to the vendored
# copy so this folder still builds on its own.
TRACE_INCLUDE = $(firstword $(wildcard ../../../TERMinator-Windows/trace/include) src)

# These flags are NOT optional decoration -- they are the build line documented
# at the top of trace_api.h. gamesandbox.c requires the module to IMPORT one
# shared memory ("env" "memory"); a module built without -pthread and
# --import-memory --shared-memory exports its own memory instead and the host
# refuses to instantiate it ("the module's memory must be shared").
CFLAGS  = --target=wasm32-wasip1-threads -O2 -DNDEBUG -std=gnu99 -Wall -Wextra \
          -pthread -I src -I $(TRACE_INCLUDE)
LDFLAGS = -mexec-model=reactor \
          -Wl,--import-memory,--export-memory,--shared-memory,--max-memory=$(MAX_MEMORY) \
          -Wl,--export=wasi_thread_start -Wl,--export-dynamic -Wl,--strip-all -lm

# 64 MB: the frame is 4.9 MB at 2x, the cloud sprites under 1 MB. The host caps
# this at 1 GB.
MAX_MEMORY = 67108864

SRC  = src/wxmodule.c
DEPS = $(SRC) src/protocol.h src/wxcommon.h src/fonts.h

all: weather.wasm

# Uses trace_mouse_mode, so it needs TERMinator 1.1.3+ (every desktop client
# has it). The door only offers TRACE when the Query reply says mouse=1.
weather.wasm: $(DEPS)
	$(CC) $(CFLAGS) $(SRC) $(LDFLAGS) -o $@
	@ls -l $@

# The generated font header is checked in; this is how it's made.
fonts:
	python3 ../tools/make_fonts.py src/fonts.h

clean:
	rm -f weather.wasm

.PHONY: all clean fonts
