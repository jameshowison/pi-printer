CC      = cc
# Prefer the source-built PAPPL 1.4 in /usr/local over the apt 1.3.1 in
# /lib/aarch64-linux-gnu.  PKG_CONFIG_PATH ensures we get 1.4 headers/flags;
# -Wl,-rpath bakes /usr/local/lib into the binary so the runtime linker finds
# libpappl.so.1 there first, regardless of ldconfig search order.
PKG_CONFIG_PATH := /usr/local/lib/pkgconfig:$(PKG_CONFIG_PATH)
export PKG_CONFIG_PATH
GIT_HASH := $(shell git rev-parse --short HEAD 2>/dev/null || echo unknown)
GIT_HASH_FILE := .git-hash-$(GIT_HASH)
CFLAGS  = -Wall -Wextra -g -DGIT_HASH=\"$(GIT_HASH)\" $(shell PKG_CONFIG_PATH=$(PKG_CONFIG_PATH) pkg-config --cflags pappl)
LIBS    = -Wl,-rpath,/usr/local/lib $(shell PKG_CONFIG_PATH=$(PKG_CONFIG_PATH) pkg-config --libs pappl)
TARGET  = hl5170dn-printer-app

SRCS = src/main.c src/driver.c src/pjl.c src/packbits.c
OBJS = $(SRCS:.c=.o)

.PHONY: all clean install pdfs

all: $(TARGET)

PDF_SRC      = chart-test.pdf
STAMPED_PDFS = tests/pdfs/chart-150dpi.pdf tests/pdfs/chart-apt.pdf tests/pdfs/chart-600dpi.pdf

tests/pdfs/chart-150dpi.pdf: $(PDF_SRC) tools/stamp-pdf.py
	mkdir -p tests/pdfs
	python3 tools/stamp-pdf.py $< $@ "150dpi-direct"
	pdftotext $@ - | grep -q "150dpi-direct"

tests/pdfs/chart-apt.pdf: $(PDF_SRC) tools/stamp-pdf.py
	mkdir -p tests/pdfs
	python3 tools/stamp-pdf.py $< $@ "APT-Mode-1024"
	pdftotext $@ - | grep -q "APT-Mode-1024"

tests/pdfs/chart-600dpi.pdf: $(PDF_SRC) tools/stamp-pdf.py
	mkdir -p tests/pdfs
	python3 tools/stamp-pdf.py $< $@ "600dpi-direct"
	pdftotext $@ - | grep -q "600dpi-direct"

pdfs: $(STAMPED_PDFS)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS)

src/main.o: src/main.c $(GIT_HASH_FILE)
	$(CC) $(CFLAGS) -c -o $@ $<

$(GIT_HASH_FILE):
	rm -f .git-hash-*
	touch $@

src/driver.o: src/driver.c src/pjl.h src/packbits.h
	$(CC) $(CFLAGS) -c -o $@ $<

src/pjl.o: src/pjl.c src/pjl.h
	$(CC) $(CFLAGS) -c -o $@ $<

src/packbits.o: src/packbits.c src/packbits.h
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) $(TARGET) .git-hash-*

install: $(TARGET)
	systemctl stop hl5170dn-printer-app || true
	killall hl5170dn-printer-app 2>/dev/null || true
	rm -f /tmp/hl5170dn-printer-app*.state
	install -m 755 $(TARGET) /usr/local/bin/
	install -m 644 hl5170dn-printer-app.service /etc/systemd/system/
	install -m 644 99-brother-hl5170dn.rules /etc/udev/rules.d/
	id -u printapp 2>/dev/null || \
	    useradd -r -M -G lp -s /usr/sbin/nologin printapp
	udevadm control --reload-rules && udevadm trigger
	systemctl daemon-reload
	systemctl restart avahi-daemon
	systemctl start hl5170dn-printer-app
