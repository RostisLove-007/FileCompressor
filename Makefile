CXX      := g++
GTK_CFLAGS := $(shell pkg-config --cflags gtk+-3.0 2>/dev/null || \
  echo "-I/usr/include/gtk-3.0 -I/usr/include/pango-1.0 \
        -I/usr/include/glib-2.0 -I/usr/lib/x86_64-linux-gnu/glib-2.0/include \
        -I/usr/include/harfbuzz -I/usr/include/freetype2 \
        -I/usr/include/libpng16 -I/usr/include/libmount -I/usr/include/blkid \
        -I/usr/include/fribidi -I/usr/include/cairo -I/usr/include/pixman-1 \
        -I/usr/include/gdk-pixbuf-2.0 -I/usr/include/x86_64-linux-gnu \
        -I/usr/include/webp -I/usr/include/gio-unix-2.0 \
        -I/usr/include/atk-1.0 -I/usr/include/at-spi2-atk/2.0 \
        -I/usr/include/at-spi-2.0 -I/usr/include/dbus-1.0 \
        -I/usr/lib/x86_64-linux-gnu/dbus-1.0/include -pthread")
GTK_LIBS := $(shell pkg-config --libs gtk+-3.0 2>/dev/null || \
  echo "-lgtk-3 -lgdk-3 -lpangocairo-1.0 -lpango-1.0 -lharfbuzz \
        -latk-1.0 -lcairo-gobject -lcairo -lgdk_pixbuf-2.0 \
        -lgio-2.0 -lgobject-2.0 -lglib-2.0") -lpthread

CXXFLAGS := -std=c++17 -O2 -Wall -Wextra -Isrc $(GTK_CFLAGS)
TARGET   := filecompressor
HEADERS  := $(wildcard src/*.hpp)

.PHONY: all clean

all: $(TARGET)

$(TARGET): src/main.cpp $(HEADERS)
	$(CXX) $(CXXFLAGS) -o $@ src/main.cpp $(GTK_LIBS)
	@echo "✓ Built: ./$(TARGET)"

clean:
	rm -f $(TARGET)
