# df-graphics
`g_src` from Dwarf Fortress, added back `PRINT_MODE: TEXT`.

## Features
 - `PRINT_MODE: TEXT` is now supported.
 - Added mouse emulation for pure keyboard users. This is also usable in Graphic mode.

## Installation
You'll need:
 - gcc
 - `fmod` 2.02 runtime headers and binaries from fmod.com
 - `zlib` installed in your system(so you'll have `minizip` headers)
 - All Dwarf Fortress dependencies(libSDL3 etc.)

To build this library, you'll need to:
 - Build `lua` in ./lua with `make`
 - Delete `lua.o` `luac.o` in ./lua/src after that.
 - Put `fmod` 2.02 headers in `fmod/inc` and libraries in `fmod/lib/$(uname -m)` (Yes I believe this can also be built on ARM64, not sure why would you want to do that tho. It might offer better compatibility and performance with `box64` this way)
 - Run `make`, and replace `libg_src_lib.so` in the root of Dwarf Fortress with our result library.

I might offer binary build and if I do I'll upload them in Releases, but these arent guaranteed to work and I suggest you building them on your machine.

## Bugs
 - You'll experience glitches(e.g. main menu out of bound) if your terminal size isnt big enough.
 - Wont play sound upon first interface load because we dont have the internal headers required for this.

These are the bugs that currently we cannot fix. There might be bugs that I havent discovered yet, you can report them in issues.

## Status
 - The game looks normal and in my limited tests it seems normal, quite stable and playable.
 - I havent tested it much since I'm still learning the control of v50+ (I've been a v47.05 player for a long time, this is quite new for me and I'm bulding my vim style keybind. If anyone have a prebuilt vim style keybind for fortress mode and adventure mode please send me.
 - Havent test it with dfhack yet.

## Limitation
 - Only support Linux, wont support Windows(and thus wont support Dwarf Fortress running in Wine)
 - Require quite big terminal size to display everything necessary.

## AI Usage
This patch is done with some level of AI help, but every line of code if human reviewed. This doesnt mean it's stable or bug free, but it's maintainable for a human.
