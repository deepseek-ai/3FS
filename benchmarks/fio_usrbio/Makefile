HF3FS_INCLUDE_DIR ?= /usr/include
HF3FS_LIB_DIR ?= /usr/lib
FIO_SRC_DIR ?= /usr/include

PLUGIN_NAME = hf3fs_usrbio
SO_NAME = ${PLUGIN_NAME}.so
SRC = ${PLUGIN_NAME}.cpp
OBJ = ${PLUGIN_NAME}.o

CXX = g++
CXXFLAGS = -fPIC -fpermissive -O3 -D_GNU_SOURCE -shared -rdynamic -I${HF3FS_INCLUDE_DIR} -I${FIO_SRC_DIR} -include config-host.h
LDFLAGS = -L${HF3FS_LIB_DIR} -lhf3fs_api_shared -Wl,-rpath=${HF3FS_LIB_DIR}

.PHONY: all clean

all: ${SO_NAME}

${SO_NAME}: ${SRC}
	${CXX} ${CXXFLAGS} $^ -o $@ ${LDFLAGS}

clean:
	rm -rf ${OBJ} ${SO_NAME}

