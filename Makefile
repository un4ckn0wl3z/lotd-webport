# Native Windows build (TDM-GCC / MinGW-w64 + SDL2)
CC      = gcc
SDL     = thirdparty/sdl2
CFLAGS  = -O2 -g -fno-strict-aliasing -DSDL_MAIN_HANDLED -I$(SDL)/include -Isrc $(EXTRA)
GENFLAGS= -O1 -fno-strict-aliasing -w -Isrc $(EXTRA)
LDFLAGS = -L$(SDL)/lib/x64 -lSDL2 -lws2_32 -lm
BUILD   ?= build

GEN_SRC = $(wildcard gen/*/rec_*.c) gen/modules.c gen/import_hle.c
SRC     = $(wildcard src/*.c)
OBJ     = $(patsubst gen/%.c,$(BUILD)/gen/%.o,$(GEN_SRC)) $(patsubst src/%.c,$(BUILD)/src/%.o,$(SRC))

# stop make from trying to regenerate makefiles via built-in rules
.DEFAULT_GOAL := all
Makefile Makefile.web: ;

all: $(BUILD)/lotdport.exe

$(BUILD)/lotdport.exe: $(OBJ)
	$(CC) -o $@ $(OBJ) $(LDFLAGS)
	cp $(SDL)/lib/x64/SDL2.dll $(BUILD)/

$(BUILD)/gen/%.o: gen/%.c src/recomp.h
	@mkdir -p $(dir $@)
	$(CC) $(GENFLAGS) -c $< -o $@

$(BUILD)/src/%.o: src/%.c $(wildcard src/*.h)
	@mkdir -p $(BUILD)/src
	$(CC) $(CFLAGS) -Wall -Wno-unused-function -Wno-misleading-indentation -c $< -o $@

clean:
	rm -rf $(BUILD)
