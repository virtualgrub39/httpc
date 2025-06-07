SRC = $(wildcard *.c)
OBJ = $(SRC:.c=.o)

HTTPC_INCS = 
HTTPC_LIBS = 

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
