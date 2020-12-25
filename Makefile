CC = /usr/bin/g++

SRCS = $(wildcard *.cpp */*.cpp)
OBJS = $(patsubst %cpp, %o, $(SRCS))


prefix=/home/loongson/backup
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

%.o:%.cpp
	$(CC) -c $^ $(INCLUDE)
clean:
	rm -f $(OBJS) $(TARGET)
run:clean all
	./$(TARGET)
