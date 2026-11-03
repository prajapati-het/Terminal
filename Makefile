# Terminator - X11 terminal emulator + shell
#
#   make            build ./terminator
#   make run        run in thread mode (default)
#   make runproc    run in process mode
#   make clean
#
# The sources are listed explicitly (not with a wildcard) so that stray files
# in the directory never end up in the build.

CC      ?= gcc
CFLAGS  ?= -O2 -std=gnu11 -Wall -Wextra -pthread
LDLIBS  ?= -lX11 -pthread

SRC = main.c state.c util.c history.c lexer.c exec.c builtins.c script.c \
      multiwatch.c shell.c worker.c bindings.c edit.c gui.c
OBJ = $(SRC:.c=.o)
HDR = terminator.h

term: $(OBJ)
	$(CC) $(CFLAGS) -o terminator $(OBJ) $(LDLIBS)

$(OBJ): %.o: %.c $(HDR)
	$(CC) $(CFLAGS) -c $< -o $@

run: term
	./terminator

runproc: term
	./terminator useProcess=True

clean:
	rm -f $(OBJ) terminator

.PHONY: run runproc clean

