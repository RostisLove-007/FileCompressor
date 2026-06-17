CXX      := g++
CXXFLAGS := -std=c++17 -O2 -Wall -Wextra \
            $(shell pkg-config --cflags gtk+-3.0) \
            -Isrc
LDFLAGS  := $(shell pkg-config --libs gtk+-3.0) -lpthread

TARGET   := filecompressor
SRCDIR   := src
SOURCES  := $(SRCDIR)/main.cpp
HEADERS  := $(SRCDIR)/bit_io.hpp \
             $(SRCDIR)/crc32.hpp \
             $(SRCDIR)/huffman.hpp \
             $(SRCDIR)/lz77.hpp \
             $(SRCDIR)/lzss.hpp \
             $(SRCDIR)/ppm.hpp \
             $(SRCDIR)/rnc.hpp \
             $(SRCDIR)/archive.hpp \
             $(SRCDIR)/file_detector.hpp \
             $(SRCDIR)/compressor.hpp

.PHONY: all clean install

all: $(TARGET)

$(TARGET): $(SOURCES) $(HEADERS)
	$(CXX) $(CXXFLAGS) -o $@ $(SOURCES) $(LDFLAGS)
	@echo "✓ Сборка успешна: ./$(TARGET)"

clean:
	rm -f $(TARGET)

install: $(TARGET)
	install -m 755 $(TARGET) /usr/local/bin/
	@echo "✓ Установлено в /usr/local/bin/$(TARGET)"
