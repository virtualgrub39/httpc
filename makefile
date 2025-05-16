SRC = $(wildcard *.c)
OBJ = $(SRC:.c=.o)

HTTPC_INCS = $(shell pkg-config --cflags gio-2.0 glib-2.0)
HTTPC_LIBS = $(shell pkg-config --libs gio-2.0 glib-2.0)

HTTPC_CFLAGS = $(CFLAGS) $(HTTPC_INCS) \
	-Wall -Wextra -pedantic -Werror \
	-ggdb
HTTPC_LDFLAGS = $(LDFLAGS) $(HTTPC_LIBS)

all: httpc

config.h: config.def.h
	cp config.def.h config.h

.c.o:
	$(CC) $(HTTPC_CFLAGS) -c $<

httpc.o: config.h

httpc: $(OBJ)
	$(CC) -o $@ $(OBJ) $(HTTPC_LDFLAGS)

clean:
	rm -f httpc $(OBJ)

commands:
	make clean; bear -- make

.PHONY: all clean commands
