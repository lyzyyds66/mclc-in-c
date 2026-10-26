# MCLC - MinGW-w64 / MSYS2 build
#
#   make
#   make clean
#
# Cross-compiling from Linux:
#   x86_64-w64-mingw32-gcc -std=c11 -O2 ... (see below)

CC      ?= gcc
CFLAGS  ?= -std=gnu11 -O2 -Wall -Wextra -Wpedantic
CPPFLAGS = -D_CRT_SECURE_NO_WARNINGS -DUNICODE -D_UNICODE -DWIN32_LEAN_AND_MEAN
LDFLAGS = -static -static-libgcc
LDLIBS  = -lwinhttp -lshell32

SRC = src/main.c src/util.c src/json.c src/hash.c src/http.c \
      src/inflate.c src/zip.c src/version.c src/install.c \
      src/remote.c src/launch.c

OBJ = $(SRC:.c=.o)

all: mclc.exe

mclc.exe: $(OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJ) $(LDLIBS)

%.o: %.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

clean:
	cmd /c "del /q src\\*.o mclc.exe 2>nul" || rm -f src/*.o mclc.exe

.PHONY: all clean
