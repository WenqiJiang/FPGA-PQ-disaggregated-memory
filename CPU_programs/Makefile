CC = g++
CLAGS=-Wall -std=c++20
LINK = -lpthread

all: FPGA_simulator.bin \
	host_single_FPGA.bin \
	# host_multi_FPGA.bin \
	# hnswlib_recall.bin \
	# hnswlib_save_load_index.bin


FPGA_simulator.bin: FPGA_simulator.cpp
	${CC} ${CLAGS} FPGA_simulator.cpp ${LINK} -o FPGA_simulator

host_single_FPGA.bin: host_single_FPGA.cpp
	${CC} ${CLAGS} host_single_FPGA.cpp ${LINK} -o host_single_FPGA

.PHONY: clean, cleanall

cleanall: clean

clean:
	rm FPGA_simulator host_single_FPGA
