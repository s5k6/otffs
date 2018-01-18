
src = $(wildcard *.c)
obj = $(patsubst %.c,%.o,$(src))
dep = $(patsubst %.c,%.d,$(src))

version = "$(shell git describe --dirty --always --tags)"

targets = cmprep parsetest manyopen

.PHONY: all clean distclean test

all : $(targets)

clean:
	rm -f $(obj) $(dep)

distclean: 
	git clean -xdf .

include $(dep)

%.d : %.c
	gcc @cflags -MM $< > $@

cmprep : cmprep.o ../fmap.o
	gcc -o $@ @cflags $^

parsetest: parsetest.o ../parser.o ../avl_tree.o ../common.o
	gcc -o $@ @cflags $^

%.o : %.c
	gcc @cflags -c $<
