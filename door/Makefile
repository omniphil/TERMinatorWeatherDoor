# Builds the Weather door. Runs on the BBS box: it fetches and caches the
# forecast and draws the ANSI view. Needs gcc and, at runtime, curl.
MAKEFLAGS += -r
.SUFFIXES:

# Sibling folders on a dev machine, ./native after `make bundle` for the BBS.
MODSRC = $(firstword $(wildcard ../module/src native/module/src))

CC     ?= gcc
CFLAGS ?= -O2 -std=gnu99 -Wall -Wextra -I $(MODSRC)

SRC = main.c wx.c json.c ansi_view.c door.c sha256.c trace_door.c
OBJ = $(SRC:%.c=obj/%.o)

TARGET = weatherdoor

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $^ -lm -o $@
	@ls -l $@

obj/%.o: %.c
	@mkdir -p obj
	$(CC) $(CFLAGS) -c $< -o $@

# Fills in whatever the sysop is missing, without clobbering anything.
install: $(TARGET)
	@test -f weather.wasm || cp ../module/weather.wasm . 2>/dev/null || \
		echo "  !! weather.wasm missing -- run make -C ../module"
	@test -f weather.ini || cp weather.ini.example weather.ini
	@mkdir -p saves cache
	@echo "Ready. Run: ./$(TARGET) <dir containing door32.sys>"

# Snapshot the module headers so door/ compiles on the BBS with nothing beside it.
bundle:
	@# A syncing filesystem can hold a folder open, so the first rm may fail.
	@# Retry once.
	rm -rf native || (sleep 2; rm -rf native)
	mkdir -p native/module/src
	cp ../module/src/protocol.h ../module/src/wxcommon.h native/module/src/
	@echo "bundled"

clean:
	rm -rf obj $(TARGET)

.PHONY: all install bundle clean
