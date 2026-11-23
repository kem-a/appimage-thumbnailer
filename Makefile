# Build and install wrapper around Cargo.
#   make && sudo make install                 PREFIX defaults to /usr/local
#   make CARGO_FLAGS=--no-default-features    build without the drop shadow

PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
DATADIR ?= $(PREFIX)/share
CARGO ?= cargo
CARGO_FLAGS ?=

BIN = target/release/appimage-thumbnailer
THUMBNAILER = $(DESTDIR)$(DATADIR)/thumbnailers/appimage-thumbnailer.thumbnailer

all:
	$(CARGO) build --release --locked $(CARGO_FLAGS)

check:
	$(CARGO) test --locked $(CARGO_FLAGS)

# Does not build, so `sudo make install` never runs cargo as root.
install:
	install -Dm755 $(BIN) $(DESTDIR)$(BINDIR)/appimage-thumbnailer
	install -d $(DESTDIR)$(DATADIR)/thumbnailers
	sed -e 's|@EXEC_PATH@|$(BINDIR)/appimage-thumbnailer|' \
	    -e 's|@TRYEXEC_PATH@|$(BINDIR)/appimage-thumbnailer|' \
	    appimage-thumbnailer.thumbnailer.in > $(THUMBNAILER)
	chmod 644 $(THUMBNAILER)

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/appimage-thumbnailer $(THUMBNAILER)

clean:
	$(CARGO) clean

.PHONY: all check install uninstall clean
