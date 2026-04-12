CC = gcc
CFLAGS = -Wall -g
LIBS = `pkg-config fuse3 --cflags --libs`

all: myfs

myfs: src/main.c src/helpers.c src/operations.c
	$(CC) $(CFLAGS) -o myfs src/main.c src/helpers.c src/operations.c $(LIBS)

clean:
	rm -f myfs