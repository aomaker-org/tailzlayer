# Path: labs/tailzlayer/Makefile
# Purpose: Build Makefile for experimental H-VMA CXL benchmark & extreme artifacts.
# Max Column: 80 Columns (comments/docs) / 120 Chars (code)

CXX := g++
CXXFLAGS := -O3 -std=c++17 -Wall -Wextra -pthread -Iinclude
EXTREME_FLAGS := -save-temps=obj -fverbose-asm
SRC := src/hvma.cpp src/telemetry_compressor.cpp src/main.cpp
TARGET := /tmp/hvma_benchmark
PYTHON := python3

.PHONY: all clean run extreme package

all: $(TARGET)

$(TARGET): $(SRC)
	$(CXX) $(CXXFLAGS) -o $@ $^

extreme: $(SRC)
	$(CXX) $(CXXFLAGS) $(EXTREME_FLAGS) -o $(TARGET) $^
	objdump -d $(TARGET) > /tmp/hvma_benchmark_disasm.txt
	nm -C $(TARGET) > /tmp/hvma_benchmark_symbols.txt

package: extreme
	$(PYTHON) scripts/package_artifacts.py /tmp/hvma_benchmark-*.s /tmp/hvma_benchmark-*.o /tmp/hvma_benchmark_disasm.txt /tmp/hvma_benchmark_symbols.txt

run: $(TARGET)
	$(TARGET)

clean:
	rm -f /tmp/hvma_benchmark* src/*.o src/*.s src/*.i

# end of file: labs/tailzlayer/Makefile
