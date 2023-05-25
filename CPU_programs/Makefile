CC = g++
CLAGS=-Wall -std=c++17
LINK = -lpthread

all: FPGA_simulator \
	# host_single_FPGA \
	# host_multi_FPGA \
	# hnswlib_recall \
	# hnswlib_save_load_index


FPGA_simulator: FPGA_simulator.cpp
	${CC} ${CLAGS} FPGA_simulator.cpp ${LINK} -o FPGA_simulator

.PHONY: clean, cleanall

cleanall: clean

clean:
# rm network_send network_send_slow network_send_per_query network_recv  query_send_thread result_recv_thread 
	rm FPGA_simulator 
# rm host_single_FPGA_same_conn host_single_FPGA_same_conn_non_blocking
