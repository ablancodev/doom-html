# DOOM for the browser: id Software's linuxdoom-1.10 engine compiled to
# WebAssembly with Emscripten.

CC      = emcc
OUT     = public

ENGINE  = $(filter-out src/engine/i_%.c,$(wildcard src/engine/*.c))
WEB     = $(wildcard src/web/*.c)
OPL     = src/opl/opl3.c
SRCS    = $(ENGINE) $(WEB) $(OPL)
OBJS    = $(patsubst src/%.c,build/%.o,$(SRCS))

CFLAGS  = -O2 -std=gnu89 -DNORMALUNIX -MMD \
          -Isrc/engine -Isrc/web -Isrc/opl $(EXTRA_CFLAGS) \
          -Wno-implicit-int -Wno-implicit-function-declaration \
          -Wno-int-conversion -Wno-incompatible-pointer-types -w

LDFLAGS = -O2 \
          -sASYNCIFY -sASYNCIFY_IGNORE_INDIRECT=1 \
          -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=64MB \
          -sINVOKE_RUN=0 -sEXIT_RUNTIME=1 \
          -sFORCE_FILESYSTEM=1 -lidbfs.js \
          -sENVIRONMENT=web \
          -sEXPORTED_FUNCTIONS=_main,_malloc,_free \
          -sEXPORTED_RUNTIME_METHODS=FS,IDBFS,ENV,callMain,HEAPF32,HEAPU8,addRunDependency,removeRunDependency

all: $(OUT)/doom.js

$(OUT)/doom.js: $(OBJS)
	$(CC) $(LDFLAGS) $(OBJS) -o $@

build/opl/%.o: src/opl/%.c
	@mkdir -p $(dir $@)
	$(CC) -O2 -c $< -o $@

build/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf build $(OUT)/doom.js $(OUT)/doom.wasm

.PHONY: all clean
-include $(OBJS:.o=.d)
