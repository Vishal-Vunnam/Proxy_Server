CC = gcc
CFLAGS = -Wall -Wextra -O2
LIBS = -lssl -lcrypto

TARGET = proxy
SRCS = proxy.c
DIRS = ./src

OPENSSL_PATH = /opt/homebrew/opt/openssl@3

all: $(TARGET)

$(TARGET): $(DIRS)/$(SRCS)
	$(CC) $(CFLAGS) $(DIRS)/$(SRCS) -o $(TARGET) $(LIBS)

# ---------- macOS build ----------
mac:
	$(CC) $(CFLAGS) \
		-I$(OPENSSL_PATH)/include \
		-L$(OPENSSL_PATH)/lib \
		$(DIRS)/$(SRCS) -o $(TARGET) \
		-lssl -lcrypto

debug:
	$(MAKE) CFLAGS="$(CFLAGS) -g -DDEBUG"

clean:
	rm -f $(TARGET)
