
src = $(wildcard *.c)
obj = $(patsubst %.c,%.o,$(src))
dep = $(patsubst %.c,%.d,$(src))

version = "$(shell git describe --dirty --always --tags)"

targets = otffs

.PHONY: all clean distclean test

all : $(targets)

clean:
	rm -f $(obj) $(dep)

distclean: 
	git clean -xdf

test : otffs cmprep 
	./test1

include $(dep)

%.d : %.c
	gcc @cflags -MM $< > $@

otffs : otffs.o fmap.o parser.o avl_tree.o common.o
	gcc -o $@ $(shell pkg-config fuse3 --libs) $^
	strip $@

cmprep : cmprep.o fmap.o
	gcc -o $@ @cflags $^

parsetest: parsetest.o parser.o avl_tree.o common.o
	gcc -o $@ @cflags $^

otffs.o : otffs.c
	gcc @cflags -DVERSION='$(version)' $(shell pkg-config fuse3 --cflags) -c $<

%.o : %.c
	gcc @cflags -c $<
