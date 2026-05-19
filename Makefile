CC      := gcc
CFLAGS  := -Wall -Wextra -Werror -std=c11 -I src -g -O2
SRC_DIR := src
BUILD   := build

SRCS := $(SRC_DIR)/main.c $(SRC_DIR)/page.c $(SRC_DIR)/comparator.c
OBJS := $(SRCS:$(SRC_DIR)/%.c=$(BUILD)/%.o)
TARGET := btree_engine

.PHONY: all clean run

all: $(TARGET)

$(BUILD)/%.o: $(SRC_DIR)/%.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $^ -o $@

$(BUILD):
	@mkdir -p $(BUILD)

run: $(TARGET)
	./$(TARGET)

clean:
	rm -rf $(BUILD) $(TARGET)
