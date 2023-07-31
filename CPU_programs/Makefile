# hacc build server default gcc version is 9.4.0, does not support semaphore in c++20
CC = /home/wejiang/.local/gcc-12.3.0/bin/g++
# CC = g++

CLAGS=-Wall -std=c++20
LINK = -lpthread
LINK_OMP = -fopenmp

all: FPGA_simulator \
	CPU_to_FPGA \
	# CPU_to_single_FPGA \
	# host_multi_FPGA \
	# hnswlib_recall \
	# hnswlib_save_load_index


FPGA_simulator: FPGA_simulator.cpp
	${CC} ${CLAGS} FPGA_simulator.cpp ${LINK} -o FPGA_simulator

CPU_to_FPGA: CPU_to_FPGA.cpp
	${CC} ${CLAGS} CPU_to_FPGA.cpp ${LINK} ${LINK_OMP} -o CPU_to_FPGA

.PHONY: clean, cleanall

cleanall: clean

clean:
	rm FPGA_simulator CPU_to_FPGA 
