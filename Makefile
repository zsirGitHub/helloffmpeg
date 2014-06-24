#made by zsir -- 2014.06.12
#first player makefile.
#to be a better man

CC = gcc
AR = ar
CFLAGS = -Wall -O -g
DFLAGS = -Lavutil -Lavformat -Lavcodec -lm -lpthread -lz -lrt `sdl-config --cflags --libs`

all:myplay

LIB_AVFORMAT = 	../libavformat/libavformat.a
LIB_AVCODEC  = 	../libavcodec/libavcodec.a
LIB_AVUTIL   = 	../libavutil/libavutil.a
LIB_SWRE     = 	../libswresample/libswresample.a
LIB_SWSCALE  = 	../libswscale/libswscale.a

INCLUDES = 	../include/
INCLUDES += ../include/libavformat
INCLUDES += ../include/libavcodec
INCLUDES += ../include/libutil
INCLUDES += ../include/libswresample
INCLUDES += ../include/libswscale


CFLAGS += $(addprefix -I ,$(INCLUDES))

LIBS = $(LIB_AVFORMAT) $(LIB_AVCODEC) $(LIB_AVUTIL) $(LIB_SWRE) $(LIB_SWSCALE)

myplay:myplay.c $(LIBS)
	$(CC) $(CFLAGS) $^ -o $@ $(DFLAGS)

prebuild:
	bash prebuild.sh

.PHONY : clean
clean:
	-rm -f myplay

