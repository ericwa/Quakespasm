#!/bin/sh

# Change this script to meet your needs and/or environment.

#TARGET=i386-mingw32msvc
#TARGET=i686-w64-mingw32
TARGET=i686-pc-mingw32
#PREFIX=/opt/cross_win32
PREFIX=/

PATH="/c/MinGW/bin:$PATH"
export PATH

MAKE_CMD=make

SDL_CONFIG=/c/SDL-1.2.15/bin/sdl-config
CC="gcc"
AS="as"
RANLIB="ranlib"
AR="ar"
WINDRES="windres"
STRIP="strip"
export PATH CC AS AR RANLIB WINDRES STRIP

exec $MAKE_CMD SDL_CONFIG=$SDL_CONFIG CC=$CC AS=$AS RANLIB=$RANLIB AR=$AR WINDRES=$WINDRES STRIP=$STRIP -f Makefile.w32 $*
