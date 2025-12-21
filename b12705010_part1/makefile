CC=gcc
CFLAGS=-Wall -Wextra -O2
TARGET=client

all: $(TARGET)

$(TARGET): client.c
	$(CC) $(CFLAGS) $< -o $@

clean:
	rm -f $(TARGET)

# 快速測試
run: $(TARGET)
	./$(TARGET) $(HOST) $(PORT)