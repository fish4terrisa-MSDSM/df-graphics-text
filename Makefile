# Dwarf-Fortress Libgraphics Makefile
# Tested on GNU/Linux (Arch Linux, x86_64 amd)

# Install Location Configuration
UNAME_M := $(shell uname -m)

ifneq (,$(filter $(UNAME_M),arm64 aarch64))
    ARCH := arm64
else
    ARCH := $(UNAME_M)
endif

INCPATH = -I/usr/include -Ifmod/inc
LIBPATH = -L/usr/lib -Lfmod/lib/$(ARCH)
LINKOS = -lSDL2 -lSDL2_image -lSDL2_mixer -lSDL2_ttf -lz -ldl -lpthread -lncurses -lfmod -rdynamic

# Compilation Settings
CC = g++ -std=c++20
CF = -Wfatal-errors -O3 -g -DDF_GLUE_CPP -shared -DCURSES -fPIC -Dviewscreen_movieplayerst=df_viewscreen_movieplayerst -DKeybindingScreen=df_KeybindingScreen -DMacroScreenLoad=df_MacroScreenLoad -DMacroScreenSave=df_MacroScreenSave -Drenderer_2d_base=df_renderer_2d_base -Drenderer_2d=df_renderer_2d

UNAME := $(shell uname)
ifeq ($(UNAME), Linux)			#Detect GNU/Linux
endif
ifeq ($(UNAME), Darwin)			#Detect MacOS
# assume dependency installation w. homebrew
INCPATH = -I/opt/homebrew/include
LIBPATH = -L/opt/homebrew/lib
CC = g++ -std=c++20
endif

.PHONY: all
all:
	@echo "Compiling Dwarf-Fortress libgraphics (libg_src_lib.so)...";
	@$(CC) $(CF) $(INCPATH) $(LIBPATH) \
    basics.cpp \
    command_line.cpp \
    dfhooks.cpp \
    enabler.cpp \
    enabler_input.cpp \
    files.cpp \
    find_files_posix.cpp \
    graphics.cpp \
    music_and_sound.cpp \
    init.cpp \
    interface.cpp \
    keybindings.cpp \
    KeybindingScreen.cpp \
    random.cpp \
    renderer_offscreen.cpp \
    resize++.cpp \
    textlines.cpp \
    textures.cpp \
    ViewBase.cpp \
    win32_compat.cpp \
		lua.cpp \
		lua/src/*.o \
	$(LINKOS) -o libg_src_lib.so
