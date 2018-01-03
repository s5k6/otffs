
CFLAGS = -std=c99 -g -Wall -Wextra -Wpedantic -Wbad-function-cast -Wconversion -Wwrite-strings -Wstrict-prototypes

SRC = $(wildcard *.c)
OBJ = $(patsubst %.c,%.o,$(SRC))

TARGETS = otffs


.PHONY: all clean distclean test

all : $(TARGETS)

clean:
	rm -f $(OBJ)

distclean: clean
	rm -f $(TARGETS)

test : all cmprep
	fusermount -u foo || true
	mkdir -p foo
	test -e foo/random || dd if=/dev/urandom of=foo/random bs=1k count=100;
	./otffs foo &
	until test -r foo/repeat_short; do sleep 0.2; done
	./cmprep foo/random foo/repeat_short
	./cmprep foo/random foo/repeat_long
	fusermount -u foo

otffs : main.c
	gcc -o $@ $(CFLAGS) $(shell pkg-config fuse3 --cflags --libs) $<

cmprep : cmprep.c
	gcc -o $@ $(CFLAGS) $<
