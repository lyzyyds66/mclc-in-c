# MCLC - MinGW-w64 / MSYS2 build
#
#   make            build CLI and GUI
#   make mclc.exe   build CLI only
#   make mclc_gui.exe
#   make clean

CC      ?= gcc
CFLAGS  ?= -std=gnu11 -O2 -Wall -Wextra -Wpedantic
CPPFLAGS = -D_CRT_SECURE_NO_WARNINGS -DUNICODE -D_UNICODE -DWIN32_LEAN_AND_MEAN
LDFLAGS = -static -static-libgcc
LDLIBS  = -lwinhttp -lshell32
GUI_LIBS = -lwinhttp -lshell32 -lcomdlg32 -lole32 -luser32 -lgdi32

CORE_SRC = src/util.c src/json.c src/hash.c src/http.c src/inflate.c \
           src/zip.c src/version.c src/install.c src/remote.c src/launch.c
CORE_OBJ = $(CORE_SRC:.c=.o)
CLI_OBJ = src/main.o $(CORE_OBJ)
GUI_OBJ = src/gui.o $(CORE_OBJ)

all: mclc.exe mclc_gui.exe

mclc.exe: $(CLI_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(CLI_OBJ) $(LDLIBS)

mclc_gui.exe: $(GUI_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -mwindows -o $@ $(GUI_OBJ) $(GUI_LIBS)

%.o: %.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

clean:
	cmd /c "del /q src\\*.o mclc.exe mclc_gui.exe 2>nul" || rm -f src/*.o mclc.exe mclc_gui.exe

.PHONY: all clean
