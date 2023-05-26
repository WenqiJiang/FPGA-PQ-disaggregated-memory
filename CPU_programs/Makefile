CC = g++
CLAGS=-Wall -std=c++17
LINK = -lpthread

all: FPGA_simulator \
	host_single_FPGA \
	# host_multi_FPGA \
	# hnswlib_recall \
	# hnswlib_save_load_index


FPGA_simulator: FPGA_simulator.cpp
	${CC} ${CLAGS} FPGA_simulator.cpp ${LINK} -o FPGA_simulator

host_single_FPGA: host_single_FPGA.cpp
	${CC} ${CLAGS} host_single_FPGA.cpp ${LINK} -o host_single_FPGA

.PHONY: clean, cleanall

cleanall: clean

clean:
	rm FPGA_simulator host_single_FPGA
