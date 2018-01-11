
CFLAGS = -std=c99 -g -Wall -Wextra -Wpedantic -Wbad-function-cast -Wconversion -Wwrite-strings -Wstrict-prototypes -Werror

SRC = $(wildcard *.c)
OBJ = $(patsubst %.c,%.o,$(SRC))
GCH = $(patsubst %,%.gch,$(wildcard *.h))

TARGETS = otffs


.PHONY: all clean distclean test

all : $(TARGETS)

clean:
	rm -f $(OBJ) $(GCH)

distclean: 
	git clean -xdf

test : all cmprep 
	./test1

otffs : otffs.o fmap.o parser.o avl_tree.o
	gcc -o $@ $(shell pkg-config fuse3 --libs) $^

cmprep : cmprep.o fmap.o
	gcc -o $@ $(CFLAGS) $^

parsetest : parsetest.o parser.o avl_tree.o

fmap.o : fmap.c fmap.h

parser.o : parser.c parser.h

otffs.o : otffs.c
	gcc -c $(CFLAGS) $(shell pkg-config fuse3 --cflags) $^

%.o : %.c
	gcc -c $(CFLAGS) $^

