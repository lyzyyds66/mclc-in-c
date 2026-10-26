# MCLC - MinGW-w64 / MSYS2 build
#
#   make            build the GUI
#   make clean

CC      ?= gcc
RC      ?= windres
CFLAGS  ?= -std=gnu11 -O2 -Wall -Wextra -Wpedantic
CPPFLAGS = -D_CRT_SECURE_NO_WARNINGS -DUNICODE -D_UNICODE -DWIN32_LEAN_AND_MEAN
LDFLAGS = -static -static-libgcc
GUI_LIBS = -lwinhttp -lshell32 -lcomdlg32 -lole32 -luser32 -lgdi32 -lcomctl32

CORE_SRC = src/util.c src/json.c src/hash.c src/http.c src/inflate.c \
           src/zip.c src/version.c src/install.c src/remote.c src/launch.c \
           src/progress.c
CORE_OBJ = $(CORE_SRC:.c=.o)
GUI_OBJ = src/gui.o src/mclc.res.o $(CORE_OBJ)

all: mclc_gui.exe

mclc_gui.exe: $(GUI_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -mwindows -o $@ $(GUI_OBJ) $(GUI_LIBS)

%.o: %.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

src/mclc.res.o: src/mclc.rc src/mclc.manifest
	$(RC) -I src src/mclc.rc -O coff -o $@

clean:
	cmd /c "del /q src\\*.o mclc_gui.exe 2>nul" || rm -f src/*.o mclc_gui.exe

.PHONY: all clean
