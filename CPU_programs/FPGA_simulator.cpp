// FPGA_simulator: simulate the behavior for a single FPGA
//   2 thread, 1 for sending results, 1 for receiving queries

// Refer to https://github.com/WenqiJiang/FPGA-ANNS-with_network/blob/master/CPU_scripts/unused/network_send.c
// Usage (e.g.): ./FPGA_simulator 10.253.74.5 5001 8881 32
//  "Usage: " << argv[0] << " <Tx (CPU) CPU_IP_addr> <Tx send_port> <Rx recv_port> <nprobe>"

// Network order:
//   Open host_single_thread (CPU) first
//   then open FPGA simulator
//   FPGA open connection -> CPU recv -> CPU send query -> FPGA recv

// Client side C/C++ program to demonstrate Socket programming 
#include <stdio.h> 
#include <stdlib.h> 
#include <sys/socket.h> 
#include <arpa/inet.h> 
#include <unistd.h> 
#include <string.h> 
#include <unistd.h>
#include <chrono>
#include <thread>
#include <iostream>
#include <vector>
#include <fstream>
#include <cassert>
#include <algorithm>

#include "utils.hpp"

#define SEND_PKG_SIZE 1024 // 1024 
#define RECV_PKG_SIZE 4096 // 1024

class FPGARetriever {

/* 
	FPGA input (recv) format:
    // Format: foe each query
    // packet 0: header (batch_size, nprobe, terminate)
    //   for the following packets, for each query
    // 		packet 1~k: cell_IDs to scan -> size = ceil(nprobe * 4 / 64) 
    // 		packet k~n: query_vectors
    // 		packet n~m: center_vectors

	FPGA output (send) format:
    // Format: for each query
    // packet 0: header (topK)
    // packet 1~k: topK results, including vec_ID (8-byte) array and dist_array (4-byte)
	//    -> size = ceil(topK * 8 / 64) + ceil(topK * 4 / 64)
*/

public:

    const size_t D;  
    const size_t TOPK; 

    const char* CPU_IP_addr; 
	const unsigned int send_port; // FPGA send, CPU receive
	const unsigned int recv_port; // FPGA recv, CPU send
	
	// states during data transfer
	int finish_recv_query_id;
	int start_send;
	int input_batch_ready; // 1 -> ready; then the result sender can start sending
	int batch_size; // within input header, only valid when input_batch_ready == True
	int nprobe;		// within input header, only valid when input_batch_ready == True
	int terminate;	// within input header, only valid when input_batch_ready == True
	int recv_batch_id;
	int send_batch_id;
	int send_batch_finish;

	// TODO: add a batch count, check the info is not from the last batch

	const size_t bit_int = 32; 
	const size_t bit_float = 32; 
	const size_t bit_long_int = 64; 
	const size_t bit_AXI = 512; 
	
	const size_t byte_AXI = 64; 

	// size in number of 512-bit AXI data packet
	//   input
	size_t AXI_size_input_header;
	size_t AXI_size_cell_IDs;
	size_t AXI_size_query_vector; 
	size_t AXI_size_center_vector; 
	//   results
	size_t AXI_size_results_header;
	size_t AXI_size_results_vec_ID;
	size_t AXI_size_results_dist;
	size_t AXI_size_results;

	// size in bytes
	size_t bytes_result_per_query;
	size_t bytes_input_per_query;

	// input & output buffers
	char* buf_dummy_results; // dummy results to send to CPU, length = single query results
	char* buf_FPGA_input;

	// constructor
    FPGARetriever(
		const size_t in_D,
		const size_t in_TOPK,
		const char* in_CPU_IP_addr,
		const unsigned int in_send_port,
		const unsigned int in_recv_port) :
		D(in_D), TOPK(in_TOPK), CPU_IP_addr(in_CPU_IP_addr), 
		send_port(in_send_port), recv_port(in_recv_port) {

		finish_recv_query_id = -1;
		start_send = 0;
		input_batch_ready = -1;
		batch_size = -1;
		nprobe = -1;
		terminate = 0;	
		recv_batch_id = -1;
		send_batch_id = -1;
		send_batch_finish = 1; // set to one to allow first recv iteration run

		// input sizes
		AXI_size_input_header = 1;
		AXI_size_query_vector = D * bit_float % bit_AXI == 0? D * bit_float / bit_AXI : D * bit_float / bit_AXI + 1; 
		AXI_size_center_vector = D * bit_float % bit_AXI == 0? D * bit_float / bit_AXI : D * bit_float / bit_AXI + 1; 

		// result sizes
		AXI_size_results_header = 1;
		AXI_size_results_vec_ID = TOPK * bit_long_int % bit_AXI == 0?
			TOPK * bit_long_int / bit_AXI : TOPK * bit_long_int / bit_AXI + 1;
		AXI_size_results_dist = TOPK * bit_float % bit_AXI == 0?
			TOPK * bit_float / bit_AXI : TOPK * bit_float / bit_AXI + 1;
		AXI_size_results = AXI_size_results_header + AXI_size_results_vec_ID + AXI_size_results_dist; 

		bytes_result_per_query = byte_AXI * AXI_size_results;
		std::cout << "bytes_result_per_query: " << bytes_result_per_query << std::endl;

		buf_dummy_results = (char*) malloc(bytes_result_per_query);
		buf_FPGA_input = (char*) malloc(bytes_input_per_query);
	}

	void thread_recv_query() { 

		printf("Printing recv_port from Thread %d\n", recv_port); 

		int server_fd, sock;
		struct sockaddr_in address;
		int opt = 1;
		int addrlen = sizeof(address);

		// Creating socket file descriptor 
		if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0)
		{
			perror("socket failed");
			exit(EXIT_FAILURE);
		}
		if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR , &opt, sizeof(opt)))
		{
			perror("setsockopt");
			exit(EXIT_FAILURE);
		}

		address.sin_family = AF_INET;
		address.sin_addr.s_addr = INADDR_ANY;
		address.sin_port = htons(recv_port);

		// Forcefully attaching socket to the recv_port 8080 
		if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0)
		{
			perror("bind failed");
			exit(EXIT_FAILURE);
		}
		if (listen(server_fd, 3) < 0)
		{
			perror("listen");
			exit(EXIT_FAILURE);
		}
		if ((sock = accept(server_fd, (struct sockaddr *)&address,
						(socklen_t*)&addrlen))<0)
		{
			perror("accept");
			exit(EXIT_FAILURE);
		}
		printf("Successfully built connection.\n");

		std::cout << "recv sock " << sock << std::endl; 
		// wait for ready
		volatile int dummy_count = 0;
		while(!start_send) { 
		    dummy_count++;
		}
		printf("Start receiving data.\n");

		////////////////   Data transfer   ////////////////


		// Should wait until the server said all the data was sent correctly,
		// otherwise the sender may send packets yet the server did not receive.

    	while (true) {

			// wait until the sender finishes the batch
			while (send_batch_id < recv_batch_id || !send_batch_finish) {}

			// recv batch header 
			char header_buf[AXI_size_input_header];
			size_t header_recv_bytes = 0;
			while (header_recv_bytes < AXI_size_input_header) {
				int recv_bytes_this_iter = AXI_size_input_header - header_recv_bytes;
				int recv_bytes = read(sock, header_buf + header_recv_bytes, recv_bytes_this_iter);
				header_recv_bytes += recv_bytes;
				if (recv_bytes == -1) {
					printf("Receiving data UNSUCCESSFUL!\n");
					return;
				}
			}
			int batch_size = decode_int(header_buf);
			int nprobe = decode_int(header_buf + 4);
			int terminate = decode_int(header_buf + 8);
			input_batch_ready = 1; // only after decoding the header
			recv_batch_id++;
			if (terminate) {
				break;
			}

			AXI_size_cell_IDs = nprobe * bit_int % bit_AXI == 0? nprobe * bit_int / bit_AXI: nprobe * bit_int / bit_AXI + 1;
			bytes_input_per_query = byte_AXI * (AXI_size_cell_IDs + AXI_size_query_vector + nprobe * AXI_size_center_vector); // not consider header 

			// recv the batch 
			for (int query_id = 0; query_id < batch_size; query_id++) {

				std::cout << "recv query_id " << query_id << std::endl;
				size_t total_recv_bytes = 0;
				while (total_recv_bytes < bytes_input_per_query) {
					int recv_bytes_this_iter = (bytes_input_per_query - total_recv_bytes) < RECV_PKG_SIZE? (bytes_input_per_query - total_recv_bytes) : RECV_PKG_SIZE;
					int recv_bytes = read(sock, buf_FPGA_input + total_recv_bytes, recv_bytes_this_iter);
					total_recv_bytes += recv_bytes;
					
					if (recv_bytes == -1) {
						printf("Receiving data UNSUCCESSFUL!\n");
						return;
					}
#ifdef DEBUG
					else {
						std::cout << "query_id: " << query_id << " recv_bytes" << total_recv_bytes << std::endl;
					}
#endif
				}
				// set shared register as soon as the first packet of the results is received
				finish_recv_query_id = query_id; 

				if (total_recv_bytes != bytes_input_per_query) {
					printf("Receiving error, receiving more bytes than a block\n");
				}
			}

			// reset state
			input_batch_ready = -1;
			batch_size = -1;
			nprobe = -1;
			terminate = 0;	
		}

		return; // end recv thread
	} 


	void thread_send_results() { 
		
		printf("Printing send_port from Thread %d\n", send_port); 

		int sock = 0; 
		struct sockaddr_in serv_addr; 

		if ((sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) 
		{ 
			printf("\n Socket creation error \n"); 
			return; 
		} 
		std::cout << "send sock " << sock << std::endl; 
	
		serv_addr.sin_family = AF_INET; 
		serv_addr.sin_port = htons(send_port); 
		
		// Convert IPv4 and IPv6 addresses from text to binary form 
		//if(inet_pton(AF_INET, "127.0.0.1", &serv_addr.sin_addr)<=0)  
		//if(inet_pton(AF_INET, "10.1.212.153", &serv_addr.sin_addr)<=0)  
		if(inet_pton(AF_INET, CPU_IP_addr, &serv_addr.sin_addr)<=0)  
		{ 
			printf("\nInvalid address/ Address not supported \n"); 
			return; 
		} 
	
		if (connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr))<0) 
		{ 
			printf("\nConnection Failed \n"); 
			return; 
		} 

		printf("Start sending data.\n");
		start_send = 1;

		////////////////   Data transfer + Select Cells   ////////////////


    	while (true) {

			// wait the next batch header being decoded
			while (!input_batch_ready || recv_batch_id <= send_batch_id) {}

			send_batch_id++;
			send_batch_finish = 0;

			if (terminate) {
				break;
			}

			for (int query_id = 0; query_id < batch_size; query_id++) {

				while (query_id > finish_recv_query_id) {}

				std::cout << "send query_id " << query_id << std::endl;

				// send data
				size_t total_sent_bytes = 0;

				while (total_sent_bytes < bytes_result_per_query) {
					int send_bytes_this_iter = (bytes_result_per_query - total_sent_bytes) < SEND_PKG_SIZE? (bytes_result_per_query - total_sent_bytes) : SEND_PKG_SIZE;
					int sent_bytes = send(sock, buf_dummy_results + total_sent_bytes, send_bytes_this_iter, 0);
					total_sent_bytes += sent_bytes;
					if (sent_bytes == -1) {
						printf("Sending data UNSUCCESSFUL!\n");
						return;
					}
#ifdef DEBUG
					else {
						printf("total sent bytes = %d\n", total_sent_bytes);
					}
#endif
				}
				if (total_sent_bytes != bytes_result_per_query) {
					printf("Sending error, sending more bytes than a block\n");
				}
			}

			send_batch_finish = 1;
		}

		return; // end send thread
	} 

	void start_recv_send_threads() {

		// start thread with member function: https://stackoverflow.com/questions/10673585/start-thread-with-member-function
		std::thread t_send(&FPGARetriever::thread_send_results, this);
		std::thread t_recv(&FPGARetriever::thread_recv_query, this);

		t_send.join();
		t_recv.join();
	}
};



int main(int argc, char const *argv[]) 
{ 
    //////////     Parameter Init     //////////
    
    std::cout << "Usage: " << argv[0] << " <1 Tx (CPU) CPU_IP_addr> <2 Tx send_port> <3 Rx recv_port> <4 D> <5 TOPK>" << std::endl;

    const char* CPU_IP_addr;
    if (argc >= 2)
    {
        CPU_IP_addr = argv[1];
    } else {
        CPU_IP_addr = "10.253.74.5"; // alveo-build-01
    }

    unsigned int send_port = 5008; // send out results
    if (argc >= 3)
    {
        send_port = strtol(argv[2], NULL, 10);
    } 

    unsigned int recv_port = 8888; // receive query
    if (argc >= 4)
    {
        recv_port = strtol(argv[3], NULL, 10);
    } 

    size_t D = 128;
    if (argc >= 5)
    {
        D = strtol(argv[4], NULL, 10);
    } 

    size_t TOPK = 100;
    if (argc >= 6)
    {
        TOPK = strtol(argv[5], NULL, 10);
    } 


	FPGARetriever retriever(
		D,
		TOPK,
		CPU_IP_addr,
		send_port,
		recv_port);

	retriever.start_recv_send_threads();

	// std::thread t_send(retriever.thread_send_results);
	// std::thread t_recv(retriever.thread_recv_query);

	// t_send.join();
	// t_recv.join();

    return 0; 
} 

