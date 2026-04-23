CC = gcc
CFLAGS = -Wall -g -Wno-format-truncation -Isrc/guards
LIBS = `pkg-config fuse3 --cflags --libs` -lzstd

all: myfs

myfs: src/main.c src/helpers.c src/operations.c src/guards/guards.c
	$(CC) $(CFLAGS) -o myfs src/main.c src/helpers.c src/operations.c src/guards/guards.c $(LIBS)

clean:
	rm -f myfs