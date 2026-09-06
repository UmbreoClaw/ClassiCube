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
CFLAGS    ?= -O2 -Wall -Wno-unused-parameter -fno-math-errno
INCLUDES   = -I$(GAME_ROOT)

SOURCES    = BlenderModel.c bm_json.c bm_gltf.c
HEADERS    = bm_json.h bm_gltf.h

all: BlenderModel.so

BlenderModel.so: $(SOURCES) $(HEADERS)
	$(CC) $(CFLAGS) $(INCLUDES) -shared -fPIC -o $@ $(SOURCES) -lm

macos: $(SOURCES) $(HEADERS)
	$(CC) $(CFLAGS) $(INCLUDES) -shared -undefined dynamic_lookup -o BlenderModel.dylib $(SOURCES)

mingw64: $(SOURCES) $(HEADERS)
	x86_64-w64-mingw32-gcc $(CFLAGS) $(INCLUDES) -shared -s -o BlenderModel.dll $(SOURCES) -L . -lClassiCube

mingw32: $(SOURCES) $(HEADERS)
	i686-w64-mingw32-gcc $(CFLAGS) $(INCLUDES) -shared -s -o BlenderModel.dll $(SOURCES) -L . -lClassiCube

# The loader test links against the object files of a "make terminal" build of the game,
# which needs no display or GPU. main() in Platform_Posix.o is renamed so the test's own main wins.
TEST_BUILD  ?= $(GAME_ROOT)/build/linux
TEST_LIBS   ?= -lX11 -lXi -lpthread -ldl -lm
TEST_OBJS    = $(filter-out $(TEST_BUILD)/src/Platform_Posix.o, $(wildcard $(TEST_BUILD)/src/*.o) $(wildcard $(TEST_BUILD)/third_party/bearssl/*.o))

test: test/test_load test/skinned.glb
	./test/test_load example/model.obj example/model.png test/skinned.glb test/skinned.gltf

test/skinned.glb: test/make_skinned.py
	python3 test/make_skinned.py

test/test_load: test/test_load.c $(SOURCES) $(HEADERS)
	objcopy --redefine-sym main=cc_main $(TEST_BUILD)/src/Platform_Posix.o test/Platform_Posix_nomain.o
	$(CC) -g -Wall -Wno-unused-parameter -Wno-unused-function $(INCLUDES) -o $@ test/test_load.c test/Platform_Posix_nomain.o $(TEST_OBJS) $(TEST_LIBS)

clean:
	rm -f BlenderModel.so BlenderModel.dylib BlenderModel.dll test/test_load test/Platform_Posix_nomain.o test/skinned.glb test/skinned.gltf test/edge.obj

.PHONY: all macos mingw64 mingw32 test clean
