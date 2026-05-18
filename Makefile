CC = gcc
CFLAGS = -Wall -g -Wno-format-truncation -Isrc/guards
LIBS = `pkg-config fuse3 --cflags --libs` -lzstd

all: myfs

myfs: src/main.c src/helpers.c src/operations.c src/guards/guards.c
	$(CC) $(CFLAGS) -o myfs src/main.c src/helpers.c src/operations.c src/guards/guards.c $(LIBS)

run: myfs
	./myfs -f mountpoint ./backing

umount:
	fusermount -u mountpoint

test:
	@chmod +x test_suite.sh
	@./test_suite.sh mountpoint backing

clean:
	rm -f myfs