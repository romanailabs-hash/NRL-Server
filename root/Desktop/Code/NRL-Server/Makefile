# Makefile for NRL-Server v1
# Custom Linux Micro-Container & NRL-Gated Execution Daemon
# Standard: C11 + POSIX 2008 + GNU extensions (for clone/pivot_root/unshare)

CC = gcc
CFLAGS = -std=c11 -O3 -mavx2 -mfma -mpopcnt -g -Wall -Wextra -Werror
CFLAGS += -D_POSIX_C_SOURCE=200809L -D_GNU_SOURCE
CFLAGS += -I include
LDFLAGS = -lrt -lm -lpthread

ROOT_DIR = $(CURDIR)
OBJ_DIR = build/objects
BIN_DIR = build/bin

OBJS = $(OBJ_DIR)/nrl_core.o $(OBJ_DIR)/nrl_gate.o $(OBJ_DIR)/nrl_sandbox.o $(OBJ_DIR)/nrl_http.o

.PHONY: all clean test directories

all: directories $(BIN_DIR)/nrl-serverd $(BIN_DIR)/nrlctl

directories:
	mkdir -p $(OBJ_DIR) $(BIN_DIR) build/rootfs

# Object files
$(OBJ_DIR)/%.o: src/%.c include/%.h
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/nrl_sandbox.o: src/nrl_sandbox.c include/nrl_sandbox.h include/nrl_server.h
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/nrl_http.o: src/nrl_http.c include/nrl_http.h include/nrl_server.h
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/server_main.o: src/server_main.c include/nrl_server.h
	$(CC) $(CFLAGS) -c $< -o $@

# Binaries
$(BIN_DIR)/nrl-serverd: $(OBJS) src/server_main.c
	$(CC) $(CFLAGS) -o $@ $(OBJS) src/server_main.c $(LDFLAGS)

$(BIN_DIR)/nrlctl: src/ctl_main.c
	$(CC) $(CFLAGS) -o $@ src/ctl_main.c $(LDFLAGS)

# Tests
test: all
	@echo "Running NRL core unit test..."
	$(CC) $(CFLAGS) -o /tmp/nrl_test tests/nrl_test.c $(OBJ_DIR)/nrl_core.o $(LDFLAGS)
	/tmp/nrl_test && rm -f /tmp/nrl_test
	@echo "All tests passed."

clean:
	rm -rf $(OBJ_DIR)/*.o $(BIN_DIR)/nrl-serverd $(BIN_DIR)/nrlctl
	@echo "Clean complete."