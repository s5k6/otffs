
CFLAGS = -std=c99 -g -Wall -Wextra -Wpedantic -Wbad-function-cast -Wconversion -Wwrite-strings -Wstrict-prototypes

SRC = $(wildcard *.c)
OBJ = $(patsubst %.c,%.o,$(SRC))

TARGETS = otffs


.PHONY: all clean distclean

all : $(TARGETS)

clean:
	rm -f $(OBJ)

distclean: clean
	rm -f $(TARGETS)



otffs : $(OBJ)
	gcc -o $@ $(shell pkg-config fuse3 --libs) $<

main.o : main.c
	gcc $(CFLAGS) -c $(shell pkg-config fuse3 --cflags) $<
