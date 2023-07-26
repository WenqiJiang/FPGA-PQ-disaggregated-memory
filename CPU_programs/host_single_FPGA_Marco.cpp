// host_single_FPGA: the complete implementation for single FPGA
//   2 thread, 1 for sending query, 1 for receiving results

// Usage (e.g.): ./host_single_FPGA 10.253.74.24 8881 5001 128 100 32 100 16 1
//  "Usage: " << argv[0] << " <1 FPGA_IP_addr> <2 C2F_port> <3 F2C_port> <4 D>
//  <5 TOPK> <6 batch_size> <7 total_batch_num> <8 nprobe> <9 window_size>" <<
//  std::endl;

// Client side C/C++ program to demonstrate Socket programming
#include <algorithm>
#include <arpa/inet.h>
#include <cassert>
#include <chrono>
#include <fstream>
#include <iostream>
#include <mutex>
#include <pthread.h>
#include <semaphore.h>
#include <semaphore>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "constants.hpp"

// #define DEBUG // uncomment to activate debug print-statements

#ifdef DEBUG
#define IF_DEBUG_DO(x)                                                                                                                                         \
  do {                                                                                                                                                         \
    x                                                                                                                                                          \
  } while (0)
#else
#define IF_DEBUG_DO(x)                                                                                                                                         \
  do {                                                                                                                                                         \
  } while (0)
#endif

#define G2C_C2F_QUEUE_SIZE 10 // size of producer-consumer-queue buffer between G2C and C2F
#define F2C_C2G_QUEUE_SIZE 10 // size of producer-consumer-queue buffer between F2C and C2G

class CPUCoordinator {

public:
  struct G2C_Request {
    int dim;
    int top_k;
    int batch_size;
    int terminate;
    float *queries;
  };

  struct C2G_Answer {
    int top_k;
    long *vector_ids;
    float *distances;
  };

  // parameters
  const size_t D;
  const size_t TOPK;
  const int batch_size;
  const int total_batch_num; // total number of batches to send
  int total_query_num;       // total number of numbers of queries
  const int nprobe;
  const int window_size; // gap between query IDs of C2F and F2C

  const char *FPGA_IP_addr;
  const unsigned int C2F_port; // FPGA recv, CPU send
  const unsigned int F2C_port; // FPGA send, CPU receive

  const char *GPU_IP_addr;
  const unsigned int G2C_port; // CPU recv, GPU send
  const unsigned int C2G_port; // CPU send, GPU recv

  // states during data transfer
  int start_F2C; // signal that F2C thread has finished setup connection
  int start_C2F; // signal that C2F thread has finished setup connection
  int start_G2C; // signal that G2C thread has finished setup connection
  int start_C2G; // signal that C2G thread has finished setup connection

  // semaphores to keep track of how many batches have been sent to FPGA and how many more we can send before the window_size is exeeded
  sem_t sem_C2F_n_queries_free_to_send;
  sem_t sem_C2F_n_queries_in_flight;
  // sem_C2F_n_queries_free_to_send + sem_C2F_n_queries_in_flight <= window_size

  // producer-consumer-queue for buffering queries received from GPU before sending them to FPGA
  std::counting_semaphore<G2C_C2F_QUEUE_SIZE> number_G2C_queueing_queries{0};
  std::counting_semaphore<G2C_C2F_QUEUE_SIZE> number_G2C_empty_positions{G2C_C2F_QUEUE_SIZE};
  std::mutex G2C_C2F_queue_manipulation;

  G2C_Request *G2C_C2F_queue;
  unsigned int G2C_rcv_index;
  unsigned int C2F_send_index;

  // producer-consumer-queue for buffering queries received from GPU before sending them to FPGA
  std::counting_semaphore<F2C_C2G_QUEUE_SIZE> number_F2C_received_answers{0};
  std::counting_semaphore<F2C_C2G_QUEUE_SIZE> number_F2C_empty_positions{F2C_C2G_QUEUE_SIZE};
  std::mutex F2C_C2G_queue_manipulation;

  C2G_Answer *F2C_C2G_queue;
  unsigned int F2C_rcv_index;
  unsigned int C2G_send_index;

  int rcv_G2C_batch_id;    // signal until what batch_id has been received from GPU
  int C2F_batch_id;        // signal until what batch of data has been sent to FPGA
  int finish_C2F_query_id; // signal until what query_id has been sent to FPGA
  int finish_F2C_query_id; // signal until what query_id has been received an answer to from FPGA
  int finish_C2G_query_id; // signal until what query_id the answer has been sent to the GPU

  // size in bytes
  size_t bytes_C2F_header;
  size_t bytes_F2C_per_query; // expected bytes received per query including header
  size_t bytes_C2F_per_query;

  // variables used for connections
  int sock_c2f;
  int sock_f2c;
  int sock_g2c;
  int sock_c2g;

  // C2F & F2C buffers, length = single batch of queries including padding
  char *buf_F2C;
  char *buf_C2F;

  // terminate signal to be sent in the packet header to FPGA
  // it is primarily part of a payload but it is also used as a signal as it affects the control flow
  // TODO: should no longer be used for signaling
  int terminate;

  std::chrono::system_clock::time_point *query_start_time_array;
  std::chrono::system_clock::time_point *query_finish_time_array;

  // constructor
  CPUCoordinator(const size_t in_D, const size_t in_TOPK, const int in_batch_size, const int in_total_batch_num, const int in_nprobe, const int in_window_size,
                 const char *in_FPGA_IP_addr, const unsigned int in_C2F_port, const unsigned int in_F2C_port, const char *in_GPU_IP_addr,
                 const unsigned int in_G2C_port, const unsigned int in_C2G_port)
      : D(in_D), TOPK(in_TOPK), batch_size(in_batch_size), total_batch_num(in_total_batch_num), nprobe(in_nprobe), window_size(in_window_size),
        FPGA_IP_addr(in_FPGA_IP_addr), C2F_port(in_C2F_port), F2C_port(in_F2C_port), GPU_IP_addr(in_GPU_IP_addr), G2C_port(in_G2C_port), C2G_port(in_C2G_port) {

    // Initialize internal variables
    total_query_num = batch_size * total_batch_num;

    start_F2C = 0;
    start_C2F = 0;
    start_C2G = 0;
    start_G2C = 0;
    finish_C2F_query_id = -1;
    finish_F2C_query_id = -1;
    rcv_G2C_batch_id = -1;
    G2C_rcv_index = 0;
    C2F_send_index = 0;
    F2C_rcv_index = 0;
    C2G_send_index = 0;
    finish_C2G_query_id = -1;
    terminate = 0;
    C2F_batch_id = -1;
    // F2C_batch_id = -1;
    // F2C_batch_finish = 1; // set to one to allow first C2F iteration run
    sem_init(&sem_C2F_n_queries_free_to_send, 1, window_size);
    sem_init(&sem_C2F_n_queries_in_flight, 1, 0);

    // C2F sizes
    bytes_C2F_header = num_packages::AXI_size_C2F_header * bit_byte_const::byte_AXI;
    bytes_C2F_per_query = bit_byte_const::byte_AXI * (num_packages::AXI_size_C2F_cell_IDs(nprobe) + num_packages::AXI_size_C2F_query_vector(D) +
                                                      nprobe * num_packages::AXI_size_C2F_center_vector(D)); // not consider header

    IF_DEBUG_DO(std::cout << "bytes_C2F_per_query (exclude 64-byte header): " << bytes_C2F_per_query << std::endl;);

    // F2C sizes
    size_t AXI_size_F2C = num_packages::AXI_size_F2C_header + num_packages::AXI_size_F2C_vec_ID(TOPK) + num_packages::AXI_size_F2C_dist(TOPK);
    bytes_F2C_per_query = bit_byte_const::byte_AXI * AXI_size_F2C; // expected bytes received per query including header

    IF_DEBUG_DO(std::cout << "bytes_F2C_per_query: " << bytes_F2C_per_query << std::endl;);

    buf_F2C = (char *)malloc(bytes_F2C_per_query);
    buf_C2F = (char *)malloc(bytes_C2F_per_query);

    G2C_C2F_queue = (G2C_Request *)malloc(G2C_C2F_QUEUE_SIZE * sizeof(G2C_Request));

    for (int i = 0; i < G2C_C2F_QUEUE_SIZE; i++) {
      G2C_C2F_queue[i].queries = (float *)malloc(batch_size * D * (bit_byte_const::bit_float / 8));
    }

    F2C_C2G_queue = (C2G_Answer *)malloc(F2C_C2G_QUEUE_SIZE * sizeof(C2G_Answer));

    for (int i = 0; i < F2C_C2G_QUEUE_SIZE; i++) {
      F2C_C2G_queue[i].vector_ids = (long *)malloc(batch_size * TOPK * (bit_byte_const::bit_long_int / 8));
      F2C_C2G_queue[i].distances = (float *)malloc(batch_size * TOPK * (bit_byte_const::bit_float / 8));
    }

    query_start_time_array = (std::chrono::system_clock::time_point *)malloc(total_query_num * sizeof(std::chrono::system_clock::time_point));
    query_finish_time_array = (std::chrono::system_clock::time_point *)malloc(total_query_num * sizeof(std::chrono::system_clock::time_point));
  }

  void connect_C2F() {
    printf("Printing C2F_port from Thread %d\n", C2F_port);

    sock_c2f = 0;
    struct sockaddr_in serv_addr;

    if ((sock_c2f = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
      printf("\n Socket creation error \n");
      return;
    }

    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(C2F_port);

    // Convert IPv4 and IPv6 addresses from text to binary form
    // if(inet_pton(AF_INET, "127.0.0.1", &serv_addr.sin_addr)<=0)
    // if(inet_pton(AF_INET, "10.1.212.153", &serv_addr.sin_addr)<=0)
    if (inet_pton(AF_INET, FPGA_IP_addr, &serv_addr.sin_addr) <= 0) {
      printf("\nInvalid address/ Address not supported \n");
      return;
    }

    if (connect(sock_c2f, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
      printf("\nConnection Failed \n");
      return;
    }

    std::cout << "C2F sock: " << sock_c2f << std::endl;
    printf("Start sending data.\n");
  }

  void send_header(char *buf_header) {
    size_t sent_header_bytes = 0;
    while (sent_header_bytes < bytes_C2F_header) {
      int C2F_bytes_this_iter = (bytes_C2F_header - sent_header_bytes) < C2F_PKG_SIZE ? (bytes_C2F_header - sent_header_bytes) : C2F_PKG_SIZE;
      int C2F_bytes = send(sock_c2f, &buf_header[sent_header_bytes], C2F_bytes_this_iter, 0);
      sent_header_bytes += C2F_bytes;
      if (C2F_bytes == -1) {
        printf("Sending data UNSUCCESSFUL!\n");
        return;
      }
    }
  }

  void send_query() {
    size_t total_C2F_bytes = 0;
    while (total_C2F_bytes < bytes_C2F_per_query) {
      int C2F_bytes_this_iter = (bytes_C2F_per_query - total_C2F_bytes) < C2F_PKG_SIZE ? (bytes_C2F_per_query - total_C2F_bytes) : C2F_PKG_SIZE;
      int C2F_bytes = send(sock_c2f, &buf_C2F[total_C2F_bytes], C2F_bytes_this_iter, 0);
      total_C2F_bytes += C2F_bytes;
      if (C2F_bytes == -1) {
        printf("Sending data UNSUCCESSFUL!\n");
        return;
      } else {
        IF_DEBUG_DO(std::cout << "total C2F bytes = " << total_C2F_bytes << std::endl;);
      }
    }
    if (total_C2F_bytes != bytes_C2F_per_query) {
      printf("Sending error, sending more bytes than a block\n");
    }
  }

  /* This method is meant to be run in a separate thread.
   * It establishes and maintains a connection to the FPGA.
   * It is resposible for sending the queries to the FPGA.
   */
  void thread_C2F() {
    // TODO: remove testing related file-operations for production
    std::ofstream CPU_Request_sent("test_logs/CPU_Request_sent.json");
    CPU_Request_sent << "[";

    while (!start_F2C) {
      // wait for ready
    }

    connect_C2F();

    start_C2F = 1;

    while (!start_C2G) {
      // wait until connection to GPU for sending answers to queries is established
    }

    ////////////////   Data transfer + Select Cells   ////////////////

    std::chrono::system_clock::time_point start = std::chrono::system_clock::now();

    // used to temporary store values received from the GPU header
    unsigned int header_batch_size;
    unsigned int header_nprobe = nprobe;
    unsigned int header_terminate = 0;

    // used to prepare the header data in the exact layout the FPGA expects
    char buf_header[bytes_C2F_header];

    // used to temporary store the complete batch of queries as received from GPU
    float *query_batch = (float *)malloc(batch_size * D * sizeof(float));
    if (query_batch == NULL) {
      perror("G2C request memory allocation for queries failed");
      exit(EXIT_FAILURE);
    }

    // used to store the data in the exact format the FPGA expects
    // => the batch of queries is splited up into individual queries and interleaved with cell-ids and corresponsing center-vectors
    // additionally padding (to fill the AXI package) needs to be added after every part of the payload
    // [cell ids] [padding] [query vector] [padding] [center_vector 1] [padding] ... [center_vector nprobe] [padding]
    int n_bytes_cell_ids = num_packages::AXI_size_C2F_cell_IDs(nprobe) * bit_byte_const::byte_AXI;
    int n_bytes_query_vector = num_packages::AXI_size_C2F_query_vector(D) * bit_byte_const::byte_AXI;
    int n_bytes_center_vectors = nprobe * num_packages::AXI_size_C2F_center_vector(D) * bit_byte_const::byte_AXI;

    char *buf_cell_ids = (char *)malloc(n_bytes_cell_ids);
    char *buf_query = (char *)malloc(n_bytes_query_vector);
    char *buf_center_vectors = (char *)malloc(n_bytes_center_vectors);

    while (!header_terminate) {

      // this semaphore controls that this thread only proceeds if queries from the GPU are available
      number_G2C_queueing_queries.acquire();
      {
        std::lock_guard<std::mutex> g(G2C_C2F_queue_manipulation);
        header_batch_size = G2C_C2F_queue[C2F_send_index % G2C_C2F_QUEUE_SIZE].batch_size;
        header_terminate = G2C_C2F_queue[C2F_send_index % G2C_C2F_QUEUE_SIZE].terminate;
        if (!header_terminate) {
          memcpy(query_batch, G2C_C2F_queue[C2F_send_index % G2C_C2F_QUEUE_SIZE].queries, batch_size * D * sizeof(float));
        }
        C2F_send_index++;
      }
      number_G2C_empty_positions.release();

      memcpy(buf_header, &header_batch_size, 4);
      memcpy(buf_header + 4, &header_nprobe, 4);
      memcpy(buf_header + 8, &header_terminate, 4);

      // this semaphore controls that the window size is adhered to
      sem_wait(&sem_C2F_n_queries_free_to_send);

      // START: TESTING RELATED CODE
      // TODO: -> remove for production or find a better suited approach...
      CPU_Request_sent << "{\"batch_size\": " << header_batch_size << ", \"nprobe\": " << header_nprobe << ", \"terminate\": " << header_terminate
                       << ", \"batch\": [";

      if (header_terminate) {
        CPU_Request_sent << "]}]";
      }
      // END: TESTING RELATED CODE

      send_header(buf_header);

      if (header_terminate) {
        break;
      }

      for (int query_id = 0; query_id < batch_size; query_id++) {

        query_start_time_array[finish_C2F_query_id + 1] = std::chrono::system_clock::now();
        float *current_query = (query_batch + (query_id * D));

        // TODO: write here cell_ids and center vectors with proper padding in corresponding buffers
        // memcpy(buf_cell_ids, cell_ids, n_bytes_cell_ids);
        memcpy(buf_query, current_query, D * bit_byte_const::bit_float / 8);
        // memcpy(buf_center_vectors, center_vectors, n_bytes_center_vectors);

        memcpy(buf_C2F, buf_cell_ids, n_bytes_cell_ids);
        memcpy(buf_C2F + n_bytes_cell_ids, buf_query, n_bytes_query_vector);
        memcpy(buf_C2F + n_bytes_cell_ids + n_bytes_query_vector, buf_center_vectors, n_bytes_center_vectors);

        // START: TESTING RELATED CODE
        // TODO: -> remove for production or find a better suited approach...
        CPU_Request_sent << "{\"cell_ids\": [";
        for (unsigned int i = 0; i < header_nprobe; i++) {
          CPU_Request_sent << i;
          if (i + 1 < header_nprobe) {
            CPU_Request_sent << ",";
          }
        }

        CPU_Request_sent << "],\"query\": [";
        for (size_t i = 0; i < D; i++) {
          CPU_Request_sent << *(current_query + i);
          if (i + 1 < D) {
            CPU_Request_sent << ",";
          }
        }

        CPU_Request_sent << "],\"center_vectors\": [";
        for (unsigned int i = 0; i < header_nprobe; i++) {
          CPU_Request_sent << "[";
          for (size_t j = 0; j < D; j++) {
            CPU_Request_sent << j + i * D;
            if (j + 1 < D) {
              CPU_Request_sent << ",";
            }
          }
          if (i + 1 < header_nprobe) {
            CPU_Request_sent << "],";
          } else {
            CPU_Request_sent << "]";
          }
        }
        if (query_id + 1 < batch_size) {
          CPU_Request_sent << "]},";
        } else {
          CPU_Request_sent << "]}]},";
        }

        // END: TESTING RELATED CODE

        send_query();
        finish_C2F_query_id++;
        IF_DEBUG_DO(std::cout << "finish_C2F_query_id: " << finish_C2F_query_id << std::endl;);
      }
      std::cout << "C2F sent batch: " << C2F_send_index << std::endl;
      sem_post(&sem_C2F_n_queries_in_flight);
    }

    // TODO: remove testing related file-operations for production
    CPU_Request_sent.close();

    std::chrono::system_clock::time_point end = std::chrono::system_clock::now();
    double durationUs = (std::chrono::duration_cast<std::chrono::microseconds>(end - start).count());

    std::cout << "C2F side Duration (us) = " << durationUs << std::endl;
    std::cout << "C2F side QPS () = " << total_query_num / (durationUs / 1000.0 / 1000.0) << std::endl;
    std::cout << "C2F side finished." << std::endl;

    return;
  }

  void connect_F2C() {
    printf("Printing F2C_port from Thread %d\n", F2C_port);

    int server_fd;
    struct sockaddr_in address;
    int opt = 1;
    int addrlen = sizeof(address);

    // Creating socket file descriptor
    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
      perror("socket failed");
      exit(EXIT_FAILURE);
    }
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))) {
      perror("setsockopt");
      exit(EXIT_FAILURE);
    }

    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(F2C_port);

    // Forcefully attaching socket to the F2C_port 8080
    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
      perror("bind failed");
      exit(EXIT_FAILURE);
    }
    if (listen(server_fd, 3) < 0) {
      perror("listen");
      exit(EXIT_FAILURE);
    }
    if ((sock_f2c = accept(server_fd, (struct sockaddr *)&address, (socklen_t *)&addrlen)) < 0) {
      perror("accept");
      exit(EXIT_FAILURE);
    }
    printf("Successfully built connection.\n");
  }

  void receive_answer_to_query() {
    size_t total_F2C_bytes = 0;
    while (total_F2C_bytes < bytes_F2C_per_query) {
      int F2C_bytes_this_iter = (bytes_F2C_per_query - total_F2C_bytes) < F2C_PKG_SIZE ? (bytes_F2C_per_query - total_F2C_bytes) : F2C_PKG_SIZE;
      int F2C_bytes = read(sock_f2c, &buf_F2C[total_F2C_bytes], F2C_bytes_this_iter);
      total_F2C_bytes += F2C_bytes;

      if (F2C_bytes == -1) {
        printf("Receiving data UNSUCCESSFUL!\n");
        return;
      } else {
        // IF_DEBUG_DO(std::cout << "query_id: " << query_id << " F2C_bytes" << total_F2C_bytes << std::endl;);
      }
    }

    if (total_F2C_bytes != bytes_F2C_per_query) {
      printf("Receiving error, receiving more bytes than a block\n");
    }
  }

  void thread_F2C() {

    connect_F2C();

    start_F2C = 1;

    std::cout << "F2C sock: " << sock_f2c << std::endl;

    while (!start_C2F) {
    }

    ///////////////////////////////////////
    //// START RECEIVING DATA /////////////
    ///////////////////////////////////////

    printf("Start receiving data.\n");

    ////////////////   Data transfer   ////////////////

    // Should wait until the server said all the data was sent correctly,
    // otherwise the C2Fer may send packets yet the server did not receive.

    size_t n_bytes_top_k = batch_size * bit_byte_const::bit_int / 8;
    size_t n_bytes_vector_ids = batch_size * TOPK * (bit_byte_const::bit_long_int / 8);
    size_t n_bytes_distances = batch_size * TOPK * (bit_byte_const::bit_float / 8);

    int *top_k_buf = (int *)malloc(n_bytes_top_k);
    long *vector_ids_buf = (long *)malloc(n_bytes_vector_ids);
    float *distances_buf = (float *)malloc(n_bytes_distances);

    size_t byte_offset_vector_ids_buf = num_packages::AXI_size_F2C_header * bit_byte_const::byte_AXI;
    size_t byte_offset_distances_buf = (num_packages::AXI_size_F2C_header + num_packages::AXI_size_F2C_vec_ID(TOPK)) * bit_byte_const::byte_AXI;

    std::chrono::system_clock::time_point start = std::chrono::system_clock::now(); // reset after recving the first query

    // TODO: find a good way to signal to this thread that terminate signal was received and when the last batch is received.
    for (int F2C_batch_id = 0; F2C_batch_id < total_batch_num; F2C_batch_id++) {

      sem_wait(&sem_C2F_n_queries_in_flight);
      for (int query_id = 0; query_id < batch_size; query_id++) {

        IF_DEBUG_DO(std::cout << "F2C query_id " << finish_F2C_query_id + 1 << std::endl;);

        if (terminate && F2C_batch_id >= C2F_batch_id) {
          // In case the terminate signal was received from GPU earlier than total_batch_num:
          // continue receiving queries until last was received then break out of the loop.
          break;
        }
        // TODO receive_header();
        receive_answer_to_query();
        // Each query received consists of [header] [topk x ids] [topk x dists] all three padded to the next AXI packet size
        // We copy all answers into the same array such that the GPU can simply interpret it at as a 2D array
        memcpy(top_k_buf + query_id, buf_F2C, 4);
        memcpy(vector_ids_buf + query_id * TOPK, buf_F2C + byte_offset_vector_ids_buf, TOPK * (bit_byte_const::bit_long_int / 8));
        memcpy(distances_buf + query_id * TOPK, buf_F2C + byte_offset_distances_buf, TOPK * (bit_byte_const::bit_float / 8));

        finish_F2C_query_id++;
        query_finish_time_array[finish_F2C_query_id] = std::chrono::system_clock::now();
      }
      sem_post(&sem_C2F_n_queries_free_to_send);
      std::cout << "F2C received batch: " << F2C_batch_id + 1 << std::endl;

      number_F2C_empty_positions.acquire();
      {
        std::lock_guard<std::mutex> g(F2C_C2G_queue_manipulation);

        memcpy(F2C_C2G_queue[F2C_rcv_index % F2C_C2G_QUEUE_SIZE].vector_ids, vector_ids_buf, n_bytes_vector_ids);
        memcpy(F2C_C2G_queue[F2C_rcv_index % F2C_C2G_QUEUE_SIZE].distances, distances_buf, n_bytes_distances);
        F2C_rcv_index++;
      }
      number_F2C_received_answers.release();
    }
    // free(vector_ids_buf);
    // free(distances_buf);

    std::chrono::system_clock::time_point end = std::chrono::system_clock::now();
    double durationUs = (std::chrono::duration_cast<std::chrono::microseconds>(end - start).count());

    std::cout << "F2C side Duration (us) = " << durationUs << std::endl;
    std::cout << "F2C side QPS = " << total_query_num / (durationUs / 1000.0 / 1000.0) << std::endl;
    std::cout << "F2C side finished." << std::endl;

    return;
  }

  void start_C2F_F2C_threads() {

    // start thread with member function:
    // https://stackoverflow.com/questions/10673585/start-thread-with-member-function
    std::thread t_F2C(&CPUCoordinator::thread_F2C, this);
    std::thread t_C2F(&CPUCoordinator::thread_C2F, this);
    std::thread t_G2C(&CPUCoordinator::thread_G2C, this);
    std::thread t_C2G(&CPUCoordinator::thread_C2G, this);

    t_F2C.join();
    t_C2F.join();
    t_G2C.join();
    t_C2G.join();
  }

  void setup_and_open_socket_G2C() {
    // TODO: open a port for the GPU server to connect
    // over this connection queries are received later
    printf("Printing G2C_port from Thread %d\n", G2C_port);

    int server_fd;
    struct sockaddr_in address;
    int opt = 1;
    int addrlen = sizeof(address);

    // Creating socket file descriptor
    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
      perror("socket failed");
      exit(EXIT_FAILURE);
    }
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))) {
      perror("setsockopt");
      exit(EXIT_FAILURE);
    }

    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(G2C_port);

    // Forcefully attaching socket to the F2C_port 8080
    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
      perror("bind failed");
      exit(EXIT_FAILURE);
    }
    if (listen(server_fd, 3) < 0) {
      perror("listen");
      exit(EXIT_FAILURE);
    }
    if ((sock_g2c = accept(server_fd, (struct sockaddr *)&address, (socklen_t *)&addrlen)) < 0) {
      perror("accept");
      exit(EXIT_FAILURE);
    }
    printf("Successfully built connection.\n");
  }

  void receive_G2C_header(G2C_Request *g2c_request_without_queries) {
    size_t n_bytes_only_header = sizeof(G2C_Request) - sizeof(float *);
    size_t G2C_bytes = read(sock_g2c, (char *)g2c_request_without_queries, n_bytes_only_header);
    if (G2C_bytes != n_bytes_only_header) {
      perror("reading header failed\n");
      exit(EXIT_FAILURE);
    }
  }

  void receive_G2C_queries(float *queries) {
    // Calculate space for the request
    int dataSize = batch_size * D * sizeof(float);

    // Read loop for receiving data
    int totalBytesReceived = 0;
    int bufferSize = 1024;
    int remainingBytes = 0;

    // Receive data in chunks until all data is received
    while (totalBytesReceived < dataSize) {
      remainingBytes = dataSize - totalBytesReceived;
      if (remainingBytes < bufferSize) {
        bufferSize = remainingBytes;
      }

      int valread = read(sock_g2c, ((char *)queries) + totalBytesReceived, bufferSize);
      if (valread < 0) {
        perror("read failed");
        exit(EXIT_FAILURE);
      }

      totalBytesReceived += valread;
    }
  }

  /*
   * Threaded-method to receive queries via network from a GPU-server.
   * Responsible for ensure they are handled correctly by the C2F threaded-method.
   */
  void thread_G2C() {
    // TODO: remove testing related file-operations for production
    std::ofstream CPU_Request_received("test_logs/CPU_Request_received.json");
    CPU_Request_received << "[";

    while (!start_F2C) {
      // wait until connection to FPGA for sending queries is established
    }

    setup_and_open_socket_G2C();
    start_G2C = 1;

    struct G2C_Request g2c_request_without_queries;
    g2c_request_without_queries.terminate = 0;  // explicitly set terminate to 0 just in is not 0 initialized
    g2c_request_without_queries.queries = NULL; // use the G2C_Request struct to only receive the header bytes
    float *queries = (float *)malloc(batch_size * D * sizeof(float));
    if (queries == NULL) {
      perror("G2C request memory allocation for queries failed");
      exit(EXIT_FAILURE);
    }
    while (!g2c_request_without_queries.terminate) {
      // receive header
      receive_G2C_header(&g2c_request_without_queries);

      // ensure that the parameters received in the header correspond to the parameters with which the service was instantiated
      assert(g2c_request_without_queries.dim == (int)D);
      assert(g2c_request_without_queries.top_k == (int)TOPK);
      assert(g2c_request_without_queries.batch_size == batch_size);
      if (!g2c_request_without_queries.terminate) {
        // receive batch of queries only if no terminate signal was sent
        receive_G2C_queries(queries);
      }

      // START: TESTING RELATED CODE
      // TODO: -> remove for production or find a better suited approach...
      CPU_Request_received << "{\"dim\": " << g2c_request_without_queries.dim << ", \"top_k\": " << g2c_request_without_queries.top_k
                           << ", \"batch_size\": " << g2c_request_without_queries.batch_size
                           << ", \"terminate_bool\": " << g2c_request_without_queries.terminate << ", \"queries\": [";
      if (!g2c_request_without_queries.terminate) {
        for (int i = 0; i < batch_size; i++) {
          CPU_Request_received << "[";
          for (int j = 0; j < (int)D; j++) {
            CPU_Request_received << *(queries + i * D + j);
            if (j + 1 < (int)D) {
              CPU_Request_received << ",";
            }
          }
          CPU_Request_received << "]";
          if (i + 1 < batch_size) {
            CPU_Request_received << ",";
          }
        }
        CPU_Request_received << "]},";
      } else {
        CPU_Request_received << "]}";
      }

      // END: TESTING RELATED CODE

      number_G2C_empty_positions.acquire();
      {
        std::lock_guard<std::mutex> g(G2C_C2F_queue_manipulation);
        memcpy(&G2C_C2F_queue[G2C_rcv_index % G2C_C2F_QUEUE_SIZE], &g2c_request_without_queries, sizeof(G2C_Request) - sizeof(float *));
        memcpy(G2C_C2F_queue[G2C_rcv_index % G2C_C2F_QUEUE_SIZE].queries, queries, batch_size * D * sizeof(float));
        G2C_rcv_index++;
      }
      number_G2C_queueing_queries.release();
      std::cout << "C2G received and queued query_batch: " << G2C_rcv_index << std::endl;
    }
    free(queries);
    // TODO: remove testing related file-operations for production
    CPU_Request_received.close();
  }

  void connect_C2G() {
    // connect to the GPU servers open port for sending answers to queries later
    printf("Printing C2G_port from Thread %d\n", C2G_port);

    sock_c2g = 0;
    struct sockaddr_in serv_addr;

    if ((sock_c2g = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
      printf("\n Socket creation error \n");
      return;
    }

    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(C2G_port);

    // Convert IPv4 and IPv6 addresses from text to binary form
    if (inet_pton(AF_INET, GPU_IP_addr, &serv_addr.sin_addr) <= 0) {
      printf("\nInvalid address/ Address not supported \n");
      return;
    }

    printf("GPU_IP_addr: %s\n", GPU_IP_addr);
    printf("C2G_port: %d\n", C2G_port);

    if (connect(sock_c2g, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
      printf("\nConnection Failed \n");
      return;
    }

    std::cout << "C2G sock: " << sock_c2g << std::endl;
    printf("Start sending data.\n");
  }

  /*
   * Threaded-method to relay answers received from the F2C threaded-method via network to a GPU-server.
   */
  void thread_C2G() {
    while (!start_G2C) {
      // wait until connection to GPU for receiving queries is established
      // This signal is only sent, when the GPU server is running and has connected to the CPU-server
      // At this state the socket to send answers back to the GPU should be open already.
    }
    connect_C2G(); // connect to port opened on GPU server
    start_C2G = 1;

    while (true) {
      number_F2C_received_answers.acquire();
      {
        std::lock_guard<std::mutex> g(F2C_C2G_queue_manipulation);

        // memcpy(&F2C_C2G_queue[F2C_rcv_index % F2C_C2G_QUEUE_SIZE].vector_ids, vector_ids_buf, n_bytes_vector_ids);
        // memcpy(&F2C_C2G_queue[F2C_rcv_index % F2C_C2G_QUEUE_SIZE].distances, distances_buf, n_bytes_distances);
        C2G_send_index++;
      }
      std::cout << "C2G_send_index: " << C2G_send_index << std::endl;
      number_F2C_empty_positions.release();
    }

    // check if thread_F2C has received answers from FPGA
    // pipe the already received queries to GPU
    return;
  }
};

int main(int argc, char const *argv[]) {
  //////////     Parameter Init     //////////

  std::cout << "Usage: " << argv[0]
            << " <1 FPGA_IP_addr> <2 C2F_port> <3 F2C_port> <4 D> <5 TOPK> <6 "
               "batch_size> <7 total_batch_num> <8 nprobe> <9 window_size> "
               "[<10 GPU_IP_addr] [<11 G2C_port] [<12 C2G_port>]"
            << std::endl;

  const char *FPGA_IP_addr;
  if (argc >= 2) {
    FPGA_IP_addr = argv[1];
  } else {
    // FPGA_IP_addr = "10.253.74.5"; // alveo-build-01
    FPGA_IP_addr = "10.253.74.12"; // alveo-u250-01
    // FPGA_IP_addr = "10.253.74.16"; // alveo-u250-02
    // FPGA_IP_addr = "10.253.74.20"; // alveo-u250-03
    // FPGA_IP_addr = "10.253.74.24"; // alveo-u250-04
  }

  unsigned int C2F_port = 8888;
  if (argc >= 3) {
    C2F_port = strtol(argv[2], NULL, 10);
  }

  unsigned int F2C_port = 5001;
  if (argc >= 4) {
    F2C_port = strtol(argv[3], NULL, 10);
  }

  size_t D = 128;
  if (argc >= 5) {
    D = strtol(argv[4], NULL, 10);
  }
  std::cout << "D: " << D << std::endl;

  size_t TOPK = 100;
  if (argc >= 6) {
    TOPK = strtol(argv[5], NULL, 10);
  }
  std::cout << "TOPK: " << TOPK << std::endl;

  int batch_size = 32;
  if (argc >= 7) {
    batch_size = strtol(argv[6], NULL, 10);
  }
  std::cout << "batch_size: " << batch_size << std::endl;

  int total_batch_num = 100;
  if (argc >= 8) {
    total_batch_num = strtol(argv[7], NULL, 10);
  }
  std::cout << "total_batch_num: " << total_batch_num << std::endl;

  int nprobe = 1;
  if (argc >= 9) {
    nprobe = strtol(argv[8], NULL, 10);
  }
  std::cout << "nprobe: " << nprobe << std::endl;

  // how many queries are allow to send ahead of receiving results
  // e.g., when window_size = 1, query 2 can be sent without receiving result 1
  // e.g., when window_size = 0, result 1 must be received before query 2 can be
  // sent, but might lead to a deadlock
  //
  // How can this deadlock?
  // => example: TODO
  int window_size = 0;
  if (argc >= 10) {
    window_size = strtol(argv[9], NULL, 10);
  }
  std::cout << "window_size: " << window_size << std::endl;

  // Arguments for integration with GPU server.

  const char *GPU_IP_addr;
  if (argc >= 11) {
    GPU_IP_addr = argv[10];
  } else {
    GPU_IP_addr = "127.0.0.1"; // for testing on localhost
  }
  std::cout << "GPU_IP_addr: " << GPU_IP_addr << std::endl;

  unsigned int G2C_port = 8889; // might be changed in the future
  if (argc >= 12) {
    G2C_port = strtol(argv[11], NULL, 10);
  }
  std::cout << "G2C_port: " << G2C_port << std::endl;

  unsigned int C2G_port = 5050; // might be changed in the future
  if (argc >= 13) {
    C2G_port = strtol(argv[12], NULL, 10);
  }
  std::cout << "C2G_port: " << C2G_port << std::endl;

  CPUCoordinator cpu_coordinator(D, TOPK, batch_size, total_batch_num, nprobe, window_size, FPGA_IP_addr, C2F_port, F2C_port, GPU_IP_addr, G2C_port, C2G_port);

  cpu_coordinator.start_C2F_F2C_threads();

  return 0;
}