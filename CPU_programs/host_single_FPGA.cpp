// host_single_FPGA: the complete implementation for single FPGA
//   2 thread, 1 for sending query, 1 for receiving results

// Usage (e.g.): ./host_single_FPGA 10.253.74.24 8881 5001 128 100 32 100 16 1
//  "Usage: " << argv[0] << " <1 FPGA_IP_addr> <2 C2F_port> <3 F2C_port> <4 D> <5 TOPK> <6 batch_size> <7 total_batch_num> <8 nprobe> <9 window_size>" << std::endl;

// Client side C/C++ program to demonstrate Socket programming 
#include <stdio.h> 
#include <stdlib.h> 
#include <stdint.h>
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

#include "constants.hpp"
// #define DEBUG


class CPUCoordinator {

public:

    const size_t D;  
    const size_t TOPK; 
    const int batch_size;
    const int total_batch_num; // total number of batches to send
    int total_query_num; // total number of numbers of queries
    const int nprobe;
    const int window_size; // gap between query IDs of C2F and F2C

    const char* FPGA_IP_addr; 
    const unsigned int F2C_port; // FPGA send, CPU receive
    const unsigned int C2F_port; // FPGA recv, CPU send

    // states during data transfer
    int start_F2C;
    int start_C2F;
    int finish_C2F_query_id; 
    int finish_F2C_query_id;

    // bit & byte const
    const size_t bit_int = 32; 
    const size_t bit_float = 32; 
    const size_t bit_long_int = 64; 
    const size_t bit_AXI = 512; 
    
    const size_t byte_AXI = 64; 

    // size in number of 512-bit AXI data packet
    //   C2F
    size_t AXI_size_C2F_header;
    size_t AXI_size_C2F_cell_IDs;
    size_t AXI_size_C2F_query_vector; 
    size_t AXI_size_C2F_center_vector; 
    //   F2C
    size_t AXI_size_F2C_header;
    size_t AXI_size_F2C_vec_ID;
    size_t AXI_size_F2C_dist;
    size_t AXI_size_F2C;

    // size in bytes
    size_t bytes_F2C_per_query;
    size_t bytes_C2F_per_query;

    // C2F & F2C buffers, length = single query
    char* buf_F2C; 
    char* buf_C2F;

    std::chrono::system_clock::time_point* query_start_time_array;
    std::chrono::system_clock::time_point* query_finish_time_array;


    // constructor
    CPUCoordinator(
        const size_t in_D,
        const size_t in_TOPK, 
        const int in_batch_size,
        const int in_total_batch_num,
        const int in_nprobe,
        const int in_window_size,
        const char* in_FPGA_IP_addr,
        const unsigned int in_C2F_port,
        const unsigned int in_F2C_port) :
        D(in_D), TOPK(in_TOPK), batch_size(in_batch_size), total_batch_num(in_total_batch_num),
        nprobe(in_nprobe), window_size(in_window_size),
        FPGA_IP_addr(in_FPGA_IP_addr), C2F_port(in_C2F_port), F2C_port(in_F2C_port) {
        
        total_query_num = batch_size * total_batch_num;

        start_F2C = 0;
        start_C2F = 0;
        finish_C2F_query_id = -1; 
        finish_F2C_query_id = -1;
        // terminate = 0;	
        // C2F_batch_id = -1;
        // F2C_batch_id = -1;
        // F2C_batch_finish = 1; // set to one to allow first C2F iteration run

        // C2F sizes
        AXI_size_C2F_header = 1;
        AXI_size_C2F_query_vector = D * bit_float % bit_AXI == 0? D * bit_float / bit_AXI : D * bit_float / bit_AXI + 1; 
        AXI_size_C2F_center_vector = D * bit_float % bit_AXI == 0? D * bit_float / bit_AXI : D * bit_float / bit_AXI + 1; 
        AXI_size_C2F_cell_IDs = nprobe * bit_int % bit_AXI == 0? nprobe * bit_int / bit_AXI: nprobe * bit_int / bit_AXI + 1;
        bytes_C2F_per_query = byte_AXI * (AXI_size_C2F_cell_IDs + AXI_size_C2F_query_vector + nprobe * AXI_size_C2F_center_vector); // not consider header 
        std::cout << "bytes_C2F_per_query: " << bytes_C2F_per_query << std::endl;

        // F2C sizes
        AXI_size_F2C_header = 1;
        AXI_size_F2C_vec_ID = TOPK * bit_long_int % bit_AXI == 0?
            TOPK * bit_long_int / bit_AXI : TOPK * bit_long_int / bit_AXI + 1;
        AXI_size_F2C_dist = TOPK * bit_float % bit_AXI == 0?
            TOPK * bit_float / bit_AXI : TOPK * bit_float / bit_AXI + 1;
        AXI_size_F2C = AXI_size_F2C_header + AXI_size_F2C_vec_ID + AXI_size_F2C_dist; 
        bytes_F2C_per_query = byte_AXI * AXI_size_F2C;
        std::cout << "bytes_F2C_per_query: " << bytes_F2C_per_query << std::endl;

        buf_F2C = (char*) malloc(bytes_F2C_per_query);
        buf_C2F = (char*) malloc(bytes_C2F_per_query);

        query_start_time_array = (std::chrono::system_clock::time_point*) malloc(total_query_num * sizeof(std::chrono::system_clock::time_point));
        query_finish_time_array = (std::chrono::system_clock::time_point*) malloc(total_query_num * sizeof(std::chrono::system_clock::time_point));
    }


    void thread_C2F() { 
        
        // wait for ready
        while(!start_F2C) {}
    
        printf("Printing C2F_port from Thread %d\n", C2F_port); 

        int sock = 0; 
        struct sockaddr_in serv_addr; 

        if ((sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) 
        { 
            printf("\n Socket creation error \n"); 
            return; 
        } 
    
        serv_addr.sin_family = AF_INET; 
        serv_addr.sin_port = htons(C2F_port); 
        
        // Convert IPv4 and IPv6 addresses from text to binary form 
        //if(inet_pton(AF_INET, "127.0.0.1", &serv_addr.sin_addr)<=0)  
        //if(inet_pton(AF_INET, "10.1.212.153", &serv_addr.sin_addr)<=0)  
        if(inet_pton(AF_INET, FPGA_IP_addr, &serv_addr.sin_addr)<=0)  
        { 
            printf("\nInvalid address/ Address not supported \n"); 
            return; 
        } 
    
        if (connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr))<0) 
        { 
            printf("\nConnection Failed \n"); 
            return; 
        } 

        std::cout << "C2F sock: " << sock << std::endl;
        printf("Start sending data.\n");
        start_C2F = 1;

        ////////////////   Data transfer + Select Cells   ////////////////

        std::chrono::system_clock::time_point start = std::chrono::system_clock::now();

        for (int C2F_batch_id = 0; C2F_batch_id < total_batch_num + 1; C2F_batch_id++) {

            std::cout << "C2F_batch_id: " << C2F_batch_id << std::endl;
            volatile int dummy_cnt = 0;
            while(finish_C2F_query_id > finish_F2C_query_id + window_size) { dummy_cnt++; }

            int batch_size;
            int nprobe;
            int terminate = C2F_batch_id == total_batch_num? 1 : 0; 
            memcpy(buf_C2F, &batch_size, 4);
            memcpy(buf_C2F + 4, &nprobe, 4);
            memcpy(buf_C2F + 8, &terminate, 4);

            for (int query_id = 0; query_id < batch_size; query_id++) {

                std::cout << "C2F query_id " << query_id << std::endl;

                query_start_time_array[finish_C2F_query_id + 1] = std::chrono::system_clock::now();

                int total_C2F_bytes = 0;
                while (total_C2F_bytes < bytes_C2F_per_query) {
                    int C2F_bytes_this_iter = (bytes_C2F_per_query - total_C2F_bytes) < C2F_PKG_SIZE? (bytes_C2F_per_query - total_C2F_bytes) : C2F_PKG_SIZE;
                    int C2F_bytes = send(sock, &buf_C2F[total_C2F_bytes], C2F_bytes_this_iter, 0);
                    total_C2F_bytes += C2F_bytes;
                    if (C2F_bytes == -1) {
                        printf("Sending data UNSUCCESSFUL!\n");
                        return;
                    }
#ifdef DEBUG
                    else {
                        printf("total C2F bytes = %d\n", total_C2F_bytes);
                    }
#endif
                }
                if (total_C2F_bytes != bytes_C2F_per_query) {
                    printf("Sending error, sending more bytes than a block\n");
                }
                finish_C2F_query_id++;
            }
        }

        std::chrono::system_clock::time_point end = std::chrono::system_clock::now();
        double durationUs = (std::chrono::duration_cast<std::chrono::microseconds>(end-start).count());
        
        std::cout << "C2F side Duration (us) = " << durationUs << std::endl;
        std::cout << "C2F side QPS () = " << total_query_num / (durationUs / 1000.0 / 1000.0) << std::endl;
        std::cout << "C2F side finished." << std::endl;
        
        return; 
    } 

    void thread_F2C() { 

        printf("Printing F2C_port from Thread %d\n", F2C_port); 

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
        address.sin_port = htons(F2C_port);

        // Forcefully attaching socket to the F2C_port 8080 
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
        start_F2C = 1; 

        std::cout << "F2C sock: " << sock << std::endl;
        while(!start_C2F) {}
        printf("Start receiving data.\n");

        ////////////////   Data transfer   ////////////////

        // Should wait until the server said all the data was sent correctly,
        // otherwise the C2Fer may send packets yet the server did not receive.

        std::chrono::system_clock::time_point start = std::chrono::system_clock::now(); // reset after recving the first query

        for (int F2C_batch_id = 0; F2C_batch_id < total_batch_num; F2C_batch_id++) {

            std::cout << "F2C_batch_id: " << F2C_batch_id << std::endl;
            for (int query_id = 0; query_id < batch_size; query_id++) {

                std::cout << "F2C query_id " << finish_F2C_query_id + 1 << std::endl;
                int total_F2C_bytes = 0;
                while (total_F2C_bytes < bytes_F2C_per_query) {
                    int F2C_bytes_this_iter = (bytes_F2C_per_query - total_F2C_bytes) < F2C_PKG_SIZE? (bytes_F2C_per_query - total_F2C_bytes) : F2C_PKG_SIZE;
                    int F2C_bytes = read(sock, &buf_F2C[total_F2C_bytes], F2C_bytes_this_iter);
                    total_F2C_bytes += F2C_bytes;
                    
                    if (F2C_bytes == -1) {
                        printf("Receiving data UNSUCCESSFUL!\n");
                        return;
                    }
#ifdef DEBUG
                    else {
                        std::cout << "query_id: " << query_id << " F2C_bytes" << total_F2C_bytes << std::endl;
                    }
#endif
                }
                finish_F2C_query_id++; 

                if (total_F2C_bytes != bytes_F2C_per_query) {
                    printf("Receiving error, receiving more bytes than a block\n");
                }
                query_finish_time_array[finish_F2C_query_id] = std::chrono::system_clock::now();
            }

            std::chrono::system_clock::time_point end = std::chrono::system_clock::now();
            double durationUs = (std::chrono::duration_cast<std::chrono::microseconds>(end-start).count());

            std::cout << "F2C side Duration (us) = " << durationUs << std::endl;
            std::cout << "F2C side QPS () = " << total_query_num / (durationUs / 1000.0 / 1000.0) << std::endl;
            std::cout << "F2C side Finished." << std::endl;

            return; 
        } 
    }

    void start_C2F_F2C_threads() {

        // start thread with member function: https://stackoverflow.com/questions/10673585/start-thread-with-member-function
        std::thread t_F2C(&CPUCoordinator::thread_F2C, this);
        std::thread t_C2F(&CPUCoordinator::thread_C2F, this);

        t_F2C.join();
        t_C2F.join();
    }

};


int main(int argc, char const *argv[]) 
{ 
    //////////     Parameter Init     //////////
    
    std::cout << "Usage: " << argv[0] << " <1 FPGA_IP_addr> <2 C2F_port> <3 F2C_port> <4 D> <5 TOPK> <6 batch_size> <7 total_batch_num> <8 nprobe> <9 window_size>" << std::endl;

    const char* FPGA_IP_addr;
    if (argc >= 2)
    {
        FPGA_IP_addr = argv[1];
    } else {
        // FPGA_IP_addr = "10.253.74.5"; // alveo-build-01
        FPGA_IP_addr = "10.253.74.12"; // alveo-u250-01
        // FPGA_IP_addr = "10.253.74.16"; // alveo-u250-02
        // FPGA_IP_addr = "10.253.74.20"; // alveo-u250-03
        // FPGA_IP_addr = "10.253.74.24"; // alveo-u250-04
    }

    unsigned int C2F_port = 8888;
    if (argc >= 3)
    {
        C2F_port = strtol(argv[2], NULL, 10);
    } 

    unsigned int F2C_port = 5001;
    if (argc >= 4)
    {
        F2C_port = strtol(argv[3], NULL, 10);
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

    int batch_size = 32;
    if (argc >= 7)
    {
        batch_size = strtol(argv[6], NULL, 10);
    } 

    int total_batch_num = 100;
    if (argc >= 8)
    {
        total_batch_num = strtol(argv[7], NULL, 10);
    } 

    int nprobe = 1;
    if (argc >= 9)
    {
        nprobe = strtol(argv[8], NULL, 10);
    } 
    
    // how many queries are allow to send ahead of receiving results
    // e.g., when window_size = 1, query 2 can be sent without receiving result 1
    // e.g., when window_size = 0, result 1 must be received before query 2 can be sent, 
    //          but might lead to a deadlock
    int window_size = 0;
    if (argc >= 10)
    {
        window_size = strtol(argv[9], NULL, 10);
    } 


    CPUCoordinator cpu_coordinator(
        D,
        TOPK, 
        batch_size,
        total_batch_num,
        nprobe,
        window_size,
        FPGA_IP_addr,
        C2F_port,
        F2C_port);

    cpu_coordinator.start_C2F_F2C_threads();

    return 0; 
} 
