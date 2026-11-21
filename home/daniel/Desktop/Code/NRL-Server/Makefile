# NRL-Server v1 Makefile
# Custom Linux Micro-Container & NRL-Gated Execution Daemon
# Targets: all | clean | test

CC = gcc
CFLAGS = -std=c11 -O2 -g -Wall -Wextra
CFLAGS += -D_POSIX_C_SOURCE=200809L -D_GNU_SOURCE
CFLAGS += -I include
LDFLAGS = -lrt -lm -lpthread

ROOT_DIR = $(CURDIR)
OBJ_DIR = build/objects
BIN_DIR = build/bin

OBJS = $(OBJ_DIR)/nrl_core.o \
       $(OBJ_DIR)/nrl_gate.o \
       $(OBJ_DIR)/nrl_sandbox.o \
       $(OBJ_DIR)/nrl_http.o

.PHONY: all clean test directories

all: directories $(BIN_DIR)/nrl-serverd $(BIN_DIR)/nrlctl

directories:
	mkdir -p $(OBJ_DIR) $(BIN_DIR) build/rootfs

$(OBJ_DIR)/nrl_core.o: src/nrl_core.c include/nrl_core.h
	$(CC) $(CFLAGS) -c src/nrl_core.c -o $(OBJ_DIR)/nrl_core.o

$(OBJ_DIR)/nrl_gate.o: src/nrl_gate.c include/nrl_gate.h include/nrl_core.h
	$(CC) $(CFLAGS) -c src/nrl_gate.c -o $(OBJ_DIR)/nrl_gate.o

$(OBJ_DIR)/nrl_sandbox.o: src/nrl_sandbox.c include/nrl_sandbox.h
	$(CC) $(CFLAGS) -c src/nrl_sandbox.c -o $(OBJ_DIR)/nrl_sandbox.o

$(OBJ_DIR)/nrl_http.o: src/nrl_http.c include/nrl_http.h
	$(CC) $(CFLAGS) -c src/nrl_http.c -o $(OBJ_DIR)/nrl_http.o

$(BIN_DIR)/nrl-serverd: $(OBJS) src/server_main.c
	$(CC) $(CFLAGS) -o $(BIN_DIR)/nrl-serverd $(OBJS) src/server_main.c $(LDFLAGS)

$(BIN_DIR)/nrlctl: src/ctl_main.c
	$(CC) $(CFLAGS) -o $(BIN_DIR)/nrlctl src/ctl_main.c $(LDFLAGS)

clean:
	rm -f $(OBJ_DIR)/*.o $(BIN_DIR)/nrl-serverd $(BIN_DIR)/nrlctl
	@echo "Clean complete."

test: all
	@echo "Testing NRL core..."
	$(BIN_DIR)/nrl-serverd --port 17999 &
	@sleep 1
	@curl -sf http://127.0.0.1:17999/health > /dev/null && echo "[PASS] /health" || echo "[FAIL] /health"
	@kill %1 2>/dev/null || true
	@echo "Tests complete."