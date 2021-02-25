CC = /usr/bin/gcc
CGLAGS=-Wall -O2 -g -std=c99
SRCS = $(wildcard *.c */*.c)
OBJS = $(patsubst %c, %o, $(SRCS))


prefix=/home/loongson/backup_wqm
#prefix=/home/wqm/backup
libdir=${prefix}/lib
includedir=${prefix}/include

INCLUDE=-I${includedir} -I${includedir}/libdrm -I${includedir}/EGL -I${includedir}/GLES2
LIBS =-L${libdir} -ldrm -lpthread -ldl -pthread -lm -lgbm -lEGL -lGLESv2 -lOSMesa

TARGET = main

.PHONY:all clean

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) -o $@ $^ $(LIBS)

%.o:%.c
	$(CC) $(CGLAGS) $(INCLUDE) -o $@ -c $<
clean:
	rm -f $(OBJS) $(TARGET)
run:clean all
	./$(TARGET)
