# Builds the Weather door. Runs on the BBS box: it fetches and caches the
# forecast, draws the ANSI view and, for JPEG XL callers, runs the TRACE module
# itself. Needs gcc and, at runtime, curl (and libjxl for the JPEG XL mode).
MAKEFLAGS += -r
.SUFFIXES:

# Sibling folders on a dev machine, ./native after `make bundle` for the BBS.
MODSRC = $(firstword $(wildcard ../module/src native/module/src))

CC     ?= gcc
CFLAGS ?= -O2 -std=gnu99 -Wall -Wextra -I $(MODSRC) -I .

SRC = main.c wx.c json.c ansi_view.c door.c sha256.c trace_door.c apc.c
OBJ = $(SRC:%.c=obj/%.o) obj/jxl_enc.o obj/pix_weather.o

TARGET = weatherdoor

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $^ -pthread -lm -ldl -o $@
	@ls -l $@

obj/%.o: %.c
	@mkdir -p obj
	$(CC) $(CFLAGS) -c $< -o $@

# libjxl is loaded at run time (dlopen); only its headers are needed here.
obj/jxl_enc.o: jxl_enc.c
	@mkdir -p obj
	$(CC) $(CFLAGS) -I jxl_include -c $< -o $@

# The TRACE module itself, compiled in for the JPEG XL mode.
obj/pix_weather.o: pix_weather.c $(wildcard $(MODSRC)/*.h $(MODSRC)/wxmodule.c)
	@mkdir -p obj
	$(CC) -O2 -std=gnu99 -pthread -Wall -Wno-attributes -Wno-unused-function -Wno-format-truncation -Wno-unknown-pragmas \
		-I $(MODSRC) -I . -c $< -o $@

# Fills in whatever the sysop is missing, without clobbering anything.
install: $(TARGET)
	@test -f weather.wasm || cp ../module/weather.wasm . 2>/dev/null || \
		echo "  !! weather.wasm missing -- run make -C ../module"
	@test -f weather.ini || cp weather.ini.example weather.ini
	@mkdir -p saves cache
	@echo "Ready. Run: ./$(TARGET) <dir containing door32.sys>"

# Snapshot the module sources (headers, and wxmodule.c for the JPEG XL mode) so door/ compiles on the BBS with nothing beside it.
bundle:
	@# A syncing filesystem can hold a folder open, so the first rm may fail.
	@# Retry once.
	rm -rf native || (sleep 2; rm -rf native)
	mkdir -p native/module/src
	cp ../module/src/*.h ../module/src/wxmodule.c native/module/src/
	@echo "bundled"

clean:
	rm -rf obj $(TARGET)

.PHONY: all install bundle clean
