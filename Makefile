CC      := gcc
CFLAGS  := -Wall -Wextra -Werror -std=gnu11 -I src -g -O2
LDFLAGS := -lpthread
SRC_DIR := src
BUILD   := build

SRCS := $(SRC_DIR)/main.c $(SRC_DIR)/page.c $(SRC_DIR)/comparator.c $(SRC_DIR)/btree.c $(SRC_DIR)/storage.c $(SRC_DIR)/buffer_pool.c $(SRC_DIR)/wal.c $(SRC_DIR)/cursor.c $(SRC_DIR)/txn.c $(SRC_DIR)/leaderboard.c $(SRC_DIR)/test.c
OBJS := $(SRCS:$(SRC_DIR)/%.c=$(BUILD)/%.o)
TARGET := btree_engine

.PHONY: all clean run

all: $(TARGET)

$(BUILD)/%.o: $(SRC_DIR)/%.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

$(BUILD):
	@mkdir -p $(BUILD)

run: $(TARGET)
	./$(TARGET)

clean:
	rm -rf $(BUILD) $(TARGET)
