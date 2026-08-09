CC = $(CROSS_COMPILE)gcc

USE_AESD_CHAR_DEVICE ?= 1

CFLAGS = -Wall -Wextra -pthread -DUSE_AESD_CHAR_DEVICE=$(USE_AESD_CHAR_DEVICE)

TARGET = aesdsocket

SRC = aesdsocket.c

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRC)

clean:
	rm -f $(TARGET)

