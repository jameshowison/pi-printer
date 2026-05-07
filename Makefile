CC     = cc
CFLAGS = -Wall -Wextra -g $(shell pkg-config --cflags pappl)
LIBS   = $(shell pkg-config --libs pappl)
TARGET = hl5170dn-printer-app

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
