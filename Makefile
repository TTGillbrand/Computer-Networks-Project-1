# Compiler and flags
CC      = gcc
CFLAGS  = -Wall -Wextra -O2 -I quirc-master/lib -pthread
LDLIBS  = -lm

# Directories
QUIRC_SRC = $(wildcard quirc-master/lib/*.c)
QUIRC_OBJ = $(QUIRC_SRC:.c=.o)

# Target executables
TARGETS = echoserver echoclient

# Default rule to build everything
all: $(TARGETS)

# Rule to build the server executable
echoserver: echoserver.o $(QUIRC_OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

# Rule to build the client executable
echoclient: echoclient.o
	$(CC) $(CFLAGS) -o $@ $^


# Generic rule to compile .c files from src/ into .o files in obj/
%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

# Order-only prerequisite to ensure obj/ directory exists
# $(OBJ_DIR):
# 	mkdir -p $(OBJ_DIR)

# Clean up build files
clean:
	rm -f *.o quirc-master/lib/*.o $(TARGETS)

#	.PHONY: all clean
