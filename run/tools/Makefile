# SPDX-License-Identifier: MIT
# Copyright (c) 2026 MoatLab, Virginia Tech
#
# Build the restart library used by the long SPEC background workloads.

CC      = gcc
CFLAGS  = -shared -fPIC -Wall -Wextra -O2
LDLIBS  = -ldl

TARGET  = restart_loop.so
SRC     = restart_loop.c

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

clean:
	rm -f $(TARGET)
