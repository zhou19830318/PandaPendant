CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -Werror -g
STATE_DIR := ../../panda_ai/components/panda_state
CPPFLAGS += -I$(STATE_DIR)/include

.PHONY: all test images clean

all: ui_sim

ui_sim: sim_main.c $(STATE_DIR)/panda_state.c $(STATE_DIR)/include/panda_state.h
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ sim_main.c $(STATE_DIR)/panda_state.c

images:
	python3 check_images.py

test: ui_sim images
	./ui_sim

clean:
	rm -f ui_sim
