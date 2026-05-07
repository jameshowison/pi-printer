CC      = cc
# Prefer the source-built PAPPL 1.4 in /usr/local over the apt 1.3.1 in
# /lib/aarch64-linux-gnu.  PKG_CONFIG_PATH ensures we get 1.4 headers/flags;
# -Wl,-rpath bakes /usr/local/lib into the binary so the runtime linker finds
# libpappl.so.1 there first, regardless of ldconfig search order.
PKG_CONFIG_PATH := /usr/local/lib/pkgconfig:$(PKG_CONFIG_PATH)
export PKG_CONFIG_PATH
CFLAGS  = -Wall -Wextra -g $(shell PKG_CONFIG_PATH=$(PKG_CONFIG_PATH) pkg-config --cflags pappl)
LIBS    = -Wl,-rpath,/usr/local/lib $(shell PKG_CONFIG_PATH=$(PKG_CONFIG_PATH) pkg-config --libs pappl)
TARGET  = hl5170dn-printer-app

SRCS = src/main.c src/driver.c src/pjl.c src/packbits.c
OBJS = $(SRCS:.c=.o)

.PHONY: all clean install

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS)

src/main.o: src/main.c
	$(CC) $(CFLAGS) -c -o $@ $<

src/driver.o: src/driver.c src/pjl.h src/packbits.h
	$(CC) $(CFLAGS) -c -o $@ $<

src/pjl.o: src/pjl.c src/pjl.h
	$(CC) $(CFLAGS) -c -o $@ $<

src/packbits.o: src/packbits.c src/packbits.h
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) $(TARGET)

install: $(TARGET)
	install -m 755 $(TARGET) /usr/local/bin/
	install -m 644 hl5170dn-printer-app.service /etc/systemd/system/
	systemctl daemon-reload
