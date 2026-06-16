CXX      := g++
CXXFLAGS := -std=c++17 -O2 -Wall -Wextra -Isrc
TARGET   := filecompressor

.PHONY: all clean

all: $(TARGET)

$(TARGET): src/main.cpp src/huffman.hpp src/bit_io.hpp src/lz77.hpp
	$(CXX) $(CXXFLAGS) -o $@ src/main.cpp
	@echo "✓ Built: ./$(TARGET)"

clean:
	rm -f $(TARGET)
