CC:=clang
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

exposway: expose.c xdg-shell-client-protocol.h xdg-shell-protocol.c
	$(CC) $(CFLAGS) \
		-o $@ $< \
		xdg-shell-protocol.c \
		$(PLIBS)

exposwayd: exposed.c
	$(CC) $(CFLAGS) \
		-o $@ $< \
		$(DLIBS)

binary: exposway exposwayd

install: exposway exposwayd
	install -s -m 755 exposwayd $(PREFIX)/bin/exposwayd
	install -s -m 755 exposway $(PREFIX)/bin/exposway

compdb: expose.c xdg-shell-client-protocol.h xdg-shell-protocol.c exposed.c
	clang -MJ expose.o.json -Wall -Wno-unused-command-line-argument -o expose.o -c expose.c \
		$(PLIBS)
	clang -MJ exposed.o.json -Wall -Wno-unused-command-line-argument -o exposed.o -c exposed.c \
		$(DLIBS)
	sed -e '1s/^/[\n/' -e '$$s/,$$/\n]/' *.o.json > compile_commands.json
	rm expose.o expose.o.json exposed.o exposed.o.json xdg-shell-client-protocol.h xdg-shell-protocol.c

analysis: expose.c xdg-shell-client-protocol.h xdg-shell-protocol.c exposed.c
	scan-build -V make CC=cc

clean:
	rm -f exposway exposwayd xdg-shell-client-protocol.h xdg-shell-protocol.c compile_commands.json

.DEFAULT_GOAL=binary
.PHONY: clean
