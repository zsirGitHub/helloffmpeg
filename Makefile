#made by zsir -- 2014.06.12
#first player makefile.
#to be a better man

CC = gcc
AR = ar
CFLAGS = -Wall -O -g
DFLAGS = -Lavutil -Lavformat -Lavcodec -lm -lpthread -lz -lrt `sdl-config --cflags --libs`

all:myPlayer

LIB_AVFORMAT = lib/libavformat.a
LIB_AVCODEC = lib/libavcodec.a
LIB_AVUTIL = lib/libavutil.a
LIB_SWRE = lib/libswresample.a
LIB_SWSCALE = lib/libswscale.a

INCLUDES = include/
INCLUDES += include/libavformat
INCLUDES += include/libavcodec
INCLUDES += include/libutil
INCLUDES += include/libswresample
INCLUDES += include/libswscale


CFLAGS += $(addprefix -I ,$(INCLUDES))

LIBS = $(LIB_AVFORMAT) $(LIB_AVCODEC) $(LIB_AVUTIL) $(LIB_SWRE) $(LIB_SWSCALE)

myPlayer:myPlayer.c $(LIBS)
	$(CC) $(CFLAGS) $^ -o $@ $(DFLAGS)

.PHONY : clean
clean:
	-rm -f myPlayer

