# MarkPeek — Linux build (GTK3 + WebKitGTK + vendored md4c)
#
#   make            build dist/markpeek
#   make run        build and run
#   make install    install to /usr/local (bin + icon + desktop entry)
#   make clean
#
# Packages (Debian/Ubuntu):  sudo apt install build-essential pkg-config \
#                              libgtk-3-dev libwebkit2gtk-4.0-dev
# Packages (Fedora):         sudo dnf install gtk3-devel webkit2gtk4.0-devel

CXX      ?= g++
CXXFLAGS ?= -O2
WARN      = -Wall -Wextra -Wno-unused-parameter
GTK       = $(shell pkg-config --cflags gtk+-3.0 webkit2gtk-4.0)
GTKLIBS   = $(shell pkg-config --libs   gtk+-3.0 webkit2gtk-4.0)

SRCDIR    = src
OUTDIR    = dist
TARGET    = $(OUTDIR)/markpeek

OBJS = md4c.o md4c-html.o entity.o main_gtk.o

all: $(TARGET)

$(TARGET): $(OBJS) | $(OUTDIR)
	$(CXX) $(OBJS) -o $@ $(GTKLIBS)

main_gtk.o: $(SRCDIR)/linux/main_gtk.cpp $(SRCDIR)/shared/shared_css.h $(SRCDIR)/shared/editor_js.h
	$(CXX) $(CXXFLAGS) $(WARN) $(GTK) -I$(SRCDIR) -c $< -o $@

md4c.o: $(SRCDIR)/md4c/md4c.c
	$(CC) $(CXXFLAGS) -O2 -c $< -o $@

md4c-html.o: $(SRCDIR)/md4c/md4c-html.c
	$(CC) $(CXXFLAGS) -O2 -c $< -o $@

entity.o: $(SRCDIR)/md4c/entity.c
	$(CC) $(CXXFLAGS) -O2 -c $< -o $@

$(OUTDIR):
	mkdir -p $(OUTDIR)

run: $(TARGET)
	./$(TARGET)

install: $(TARGET)
	install -Dm755 $(TARGET) $(DESTDIR)/usr/local/bin/markpeek
	install -Dm644 assets/icon.png $(DESTDIR)/usr/local/share/icons/hicolor/256x256/apps/markpeek.png
	install -Dm644 assets/markpeek.desktop $(DESTDIR)/usr/local/share/applications/markpeek.desktop
	-update-desktop-database /usr/local/share/applications || true

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: all run install clean
