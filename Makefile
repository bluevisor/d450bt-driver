FILTER    = rastertod450bt
CC        = clang
CFLAGS    = -O2 -Wall -Wextra -Wno-deprecated-declarations
ARCHFLAGS = -arch arm64 -arch x86_64
LIBS      = -lcups -lcupsimage

FILTER_DIR = /usr/libexec/cups/filter
PPD_DIR    = /Library/Printers/PPDs/Contents/Resources

all: build/$(FILTER)

build/$(FILTER): src/$(FILTER).c src/minilzo/minilzo.c
	mkdir -p build
	$(CC) $(CFLAGS) $(ARCHFLAGS) -o $@ src/$(FILTER).c src/minilzo/minilzo.c $(LIBS)

install: build/$(FILTER)
	sudo install -o root -g wheel -m 755 build/$(FILTER) $(FILTER_DIR)/$(FILTER)
	sudo install -o root -g wheel -m 644 ppd/D450BT.ppd "$(PPD_DIR)/D450BT.ppd"

clean:
	rm -rf build

.PHONY: all install clean
