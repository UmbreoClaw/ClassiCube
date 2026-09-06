# Builds the BlenderModel plugin.
#
#   make            - Linux / BSD shared object (BlenderModel.so)
#   make macos      - macOS dylib
#   make mingw64    - Windows 64 bit DLL, needs libClassiCube.a (see doc/plugin-dev.md)
#   make mingw32    - Windows 32 bit DLL, needs libClassiCube.a
#   make test       - builds and runs the loader test against a headless game build
#
# The game's headers are found through GAME_ROOT (the repository root by default).
GAME_ROOT ?= ../..
CC        ?= cc
CFLAGS    ?= -O2 -Wall -Wno-unused-parameter
INCLUDES   = -I$(GAME_ROOT)

all: BlenderModel.so

BlenderModel.so: BlenderModel.c
	$(CC) $(CFLAGS) $(INCLUDES) -shared -fPIC -o $@ $<

macos: BlenderModel.c
	$(CC) $(CFLAGS) $(INCLUDES) -shared -undefined dynamic_lookup -o BlenderModel.dylib $<

mingw64: BlenderModel.c
	x86_64-w64-mingw32-gcc $(CFLAGS) $(INCLUDES) -shared -s -o BlenderModel.dll $< -L . -lClassiCube

mingw32: BlenderModel.c
	i686-w64-mingw32-gcc $(CFLAGS) $(INCLUDES) -shared -s -o BlenderModel.dll $< -L . -lClassiCube

# The loader test links against the object files of a "make terminal" build of the game,
# which needs no display or GPU. main() in Platform_Posix.o is renamed so the test's own main wins.
TEST_BUILD  ?= $(GAME_ROOT)/build/linux
TEST_LIBS   ?= -lX11 -lXi -lpthread -ldl -lm
TEST_OBJS    = $(filter-out $(TEST_BUILD)/src/Platform_Posix.o, $(wildcard $(TEST_BUILD)/src/*.o) $(wildcard $(TEST_BUILD)/third_party/bearssl/*.o))

test: test/test_load
	./test/test_load example/model.obj example/model.png

test/test_load: test/test_load.c BlenderModel.c
	objcopy --redefine-sym main=cc_main $(TEST_BUILD)/src/Platform_Posix.o test/Platform_Posix_nomain.o
	$(CC) -g -Wall -Wno-unused-parameter -Wno-unused-function $(INCLUDES) -o $@ test/test_load.c test/Platform_Posix_nomain.o $(TEST_OBJS) $(TEST_LIBS)

clean:
	rm -f BlenderModel.so BlenderModel.dylib BlenderModel.dll test/test_load test/Platform_Posix_nomain.o

.PHONY: all macos mingw64 mingw32 test clean
