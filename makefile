CC:=clang
SU:=doas
PREFIX:=/usr/local
CFLAGS+=-O3 -Wno-unused-result

WAYLAND_PROTOCOLS:=$(shell pkg-config --variable=pkgdatadir wayland-protocols)
WAYLAND_SCANNER:=$(shell pkg-config --variable=wayland_scanner wayland-scanner)
PLIBS:=\
	$(shell pkg-config --cflags --libs wayland-client) \
	$(shell pkg-config --cflags --libs pangocairo) \
	-lxkbcommon \
	-lm
DLIBS:=\
	$(shell pkg-config --cflags --libs json-c)

xdg-shell-client-protocol.h:
	$(WAYLAND_SCANNER) client-header \
		$(WAYLAND_PROTOCOLS)/stable/xdg-shell/xdg-shell.xml $@

xdg-shell-protocol.c:
	$(WAYLAND_SCANNER) private-code \
		$(WAYLAND_PROTOCOLS)/stable/xdg-shell/xdg-shell.xml $@

exposway: expose.c expose.h xdg-shell-client-protocol.h xdg-shell-protocol.c
	$(CC) $(CFLAGS) \
		-o $@ $< \
		xdg-shell-protocol.c \
		$(PLIBS)

exposwayd: exposed.c
	$(CC) $(CFLAGS) \
		-o $@ $< \
		$(DLIBS)

install: exposway exposwayd
	$(SU) install -s -m 755 exposwayd $(PREFIX)/bin/exposwayd
	$(SU) install -s -m 755 exposway $(PREFIX)/bin/exposway

compdb: exposway
	clang -MJ expose.o.json -Wall -Wno-unused-command-line-argument -o expose.o -c expose.c \
		$(LIBS)
	sed -e '1s/^/[\n/' -e '$$s/,$$/\n]/' *.o.json > compile_commands.json
	rm expose.o expose.o.json

clean:
	rm -f exposway exposwayd xdg-shell-client-protocol.h xdg-shell-protocol.c compile_commands.json

.DEFAULT_GOAL=install
.PHONY: clean
