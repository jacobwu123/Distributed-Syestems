CC=/usr/bin/gcc
CC_OPTS=-g3
CC_LIBS=
CC_DEFINES=
CC_INCLUDES=
CC_ARGS=${CC_OPTS} ${CC_LIBS} ${CC_DEFINES} ${CC_INCLUDES}

# clean is not a file
.PHONY=clean

eventual: main.c
	@${CC} ${CC_ARGS} -o eventual main.c -lpthread

linearizability: linearizability.c
	@${CC} ${CC_ARGS} -o linearizability linearizability.c -lpthread

clean:
	@rm -f linearizability *.o