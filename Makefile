CXX      = g++
CXXFLAGS = -std=c++20 -Wall -Wextra -Iinclude
SRCS     = src/crc32.cpp src/wal.cpp src/vlog.cpp src/sstable.cpp src/memtable.cpp src/manifest.cpp src/compaction.cpp src/vlog_gc.cpp src/bloom.cpp src/benchmark.cpp src/cli.cpp src/kvstore.cpp src/verification.cpp src/experiment_lab.cpp src/http_server.cpp main.cpp
TARGET   = flsm

ifeq ($(OS),Windows_NT)
	TARGET  := $(TARGET).exe
	RM       = del /Q
	RMDIR    = rmdir /S /Q
	LDFLAGS  = -lws2_32
else
	RM       = rm -f
	RMDIR    = rm -rf
	LDFLAGS  =
endif

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

web: $(TARGET)
	./$(TARGET) web

clean:
	$(RM) $(TARGET) test_wal.bin
	$(RMDIR) test_flsm 2>nul || true

.PHONY: all clean web
