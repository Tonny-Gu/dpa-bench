NVCC ?= nvcc
NVCCFLAGS ?= -O3 -std=c++17

TARGET := p2p_memcpy_bench
SRC := p2p_memcpy_bench.cu

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(SRC)
	$(NVCC) $(NVCCFLAGS) -o $@ $<

clean:
	rm -f $(TARGET)
