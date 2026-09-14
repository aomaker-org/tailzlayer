# Path: labs/tailzlayer/Makefile
# Purpose: Build Makefile for experimental H-VMA CXL benchmark.
# Max Column: 80 Columns (comments/docs) / 120 Chars (code)

CXX := g++
CXXFLAGS := -O3 -std=c++17 -Wall -Wextra -pthread -Iinclude
SRC := src/hvma.cpp src/telemetry_compressor.cpp src/main.cpp
TARGET := /tmp/hvma_benchmark

.PHONY: all clean run

all: $(TARGET)

$(TARGET): $(SRC)
	$(CXX) $(CXXFLAGS) -o $@ $^

run: $(TARGET)
	$(TARGET)

clean:
	rm -f /tmp/hvma_benchmark

# end of file: labs/tailzlayer/Makefile
