VPATH = . cat_png_functions
CC = gcc       # compiler
CFLAGS = -Wall -g -std=gnu99 # compilation flags
LD = gcc       # linker
LDFLAGS = -g   # debugging symbols in build
LDLIBS = -lz -lcurl -pthread
SRCS = pnginfo.c crc.c zutil.c 
TARGET = paster2
all: $(TARGET)
paster2: paster2.c $(SRCS)
	$(CC) $(CFLAGS) $^ -o $@ $(LDLIBS) $(LDFLAGS) 
.PHONY: clean
clean:
	rm -f $(TARGET)  *.png