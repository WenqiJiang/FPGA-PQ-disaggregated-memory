/* Host CPU communicates with one or multiple FPGAs
   2 thread, 1 for sending query, 1 for receiving results

 Usage (e.g.):

  std::cout << "Usage: " << argv[0] << " <1 num_FPGA> "
  	"<2 ~ 2 + num_FPGA - 1 FPGA_IP_addr> " 
    "<2 + num_FPGA ~ 2 + 2 * num_FPGA - 1 C2F_port> " 
    "<2 + 2 * num_FPGA ~ 2 + 3 * num_FPGA - 1 F2C_port> "
    "<2 + 3 * num_FPGA D> <3 + 3 * num_FPGA TOPK> <4 + 3 * num_FPGA batch_size> "
    "<5 + 3 * num_FPGA total_batch_num> <6 + 3 * num_FPGA nprobe> "
    "<7 + 3 * num_FPGA window_size> <8 + 3 * num_FPGA enable_inter_batch_window>" << std::endl;

  Single FPGA example:
     ./CPU_to_FPGA 1 10.253.74.24 8881 5001 128 100 32 100 16 10 0
  
  Two FPGAs example:
     ./CPU_to_FPGA 2 10.253.74.24 10.253.74.28 8881 8882 5001 5002 128 100 32 100 16 10 0
*/

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

#define MAX_FPGA_NUM 16

class CPUCoordinator {

public:
  // parameters
  const size_t D;
  const size_t TOPK;
  const int batch_size;
  const int total_batch_num; // total number of batches to send
  int total_query_num;       // total number of numbers of queries
  const int nprobe;
  const int window_size; // gap between query IDs of C2F and F2C
  const int enable_inter_batch_window; // whether enable inter-batch pipeline overlap (0 = low latency; 1 = hgih throughput)

  const int num_FPGA; // <= MAX_FPGA_NUM

  // arrays of FPGA IP addresses and ports
  const char** FPGA_IP_addr;
  const unsigned int* C2F_port; // FPGA recv, CPU send
  const unsigned int* F2C_port; // FPGA send, CPU receive

  // states during data transfer
  int start_F2C; // signal that F2C thread has finished setup connection
  int start_C2F; // signal that C2F thread has finished setup connection

  // semaphores to keep track of how many batches have been sent to FPGA and how many more we can send before the window_size is exeeded
  sem_t sem_C2F_n_queries_free_to_send;
  sem_t sem_C2F_n_queries_in_flight;
  sem_t sem_C2F_batch_free_to_send;

  unsigned int C2F_send_index;
  unsigned int F2C_rcv_index;

  int C2F_batch_id;        // signal until what batch of data has been sent to FPGA
  int finish_C2F_query_id; 
  int finish_F2C_query_id;

  // size in bytes
  size_t bytes_C2F_header;
  size_t bytes_F2C_per_query; // expected bytes received per query including header
  size_t bytes_C2F_per_query;

  // variables used for connections
  int* sock_c2f;
  int* sock_f2c;

  // C2F & F2C buffers, length = single batch of queries including padding
  char* buf_F2C;
  char* buf_C2F;

  // terminate signal to be sent in the packet header to FPGA
  // it is primarily part of a payload but it is also used as a signal as it affects the control flow
  // TODO: should no longer be used for signaling
  int terminate;

  std::chrono::system_clock::time_point* batch_start_time_array;
  std::chrono::system_clock::time_point* batch_finish_time_array;
  double* batch_duration_ms_array;

  // constructor
  CPUCoordinator(
    const size_t in_D,
    const size_t in_TOPK, 
    const int in_batch_size,
    const int in_total_batch_num,
    const int in_nprobe,
    const int in_window_size,
    const int in_enable_inter_batch_window,
    const int in_num_FPGA,
    const char** in_FPGA_IP_addr,
    const unsigned int* in_C2F_port,
    const unsigned int* in_F2C_port) :
    D(in_D), TOPK(in_TOPK), batch_size(in_batch_size), total_batch_num(in_total_batch_num),
    nprobe(in_nprobe), window_size(in_window_size), enable_inter_batch_window(in_enable_inter_batch_window),
    num_FPGA(in_num_FPGA), FPGA_IP_addr(in_FPGA_IP_addr), C2F_port(in_C2F_port), F2C_port(in_F2C_port) {
        
    // Initialize internal variables
    total_query_num = batch_size * total_batch_num;

    start_F2C = 0;
    start_C2F = 0;
    finish_C2F_query_id = -1; 
    finish_F2C_query_id = -1;
    C2F_send_index = 0;
    F2C_rcv_index = 0;
    terminate = 0;
    C2F_batch_id = -1;
    // F2C_batch_id = -1;
    // F2C_batch_finish = 1; // set to one to allow first C2F iteration run
    sem_init(&sem_C2F_n_queries_free_to_send, 0, window_size); // 0 = share between threads of a process
    sem_init(&sem_C2F_n_queries_in_flight, 0, 0); // 0 = share between threads of a process
    sem_init(&sem_C2F_batch_free_to_send, 0, 1); // 0 = share between threads of a process

    // C2F sizes
    bytes_C2F_header = num_packages::AXI_size_C2F_header * bit_byte_const::byte_AXI;
    bytes_C2F_per_query = bit_byte_const::byte_AXI * (num_packages::AXI_size_C2F_cell_IDs(nprobe) + num_packages::AXI_size_C2F_query_vector(D) +
                                                      nprobe * num_packages::AXI_size_C2F_center_vector(D)); // not consider header

    IF_DEBUG_DO(std::cout << "bytes_C2F_per_query (exclude 64-byte header): " << bytes_C2F_per_query << std::endl;);

    // F2C sizes
    size_t AXI_size_F2C = num_packages::AXI_size_F2C_header + num_packages::AXI_size_F2C_vec_ID(TOPK) + num_packages::AXI_size_F2C_dist(TOPK);
    bytes_F2C_per_query = bit_byte_const::byte_AXI * AXI_size_F2C; // expected bytes received per query including header

    IF_DEBUG_DO(std::cout << "bytes_F2C_per_query: " << bytes_F2C_per_query << std::endl;);

    sock_f2c = (int*) malloc(num_FPGA * sizeof(int));
    sock_c2f = (int*) malloc(num_FPGA * sizeof(int));
    
    buf_F2C = (char*) malloc(bytes_F2C_per_query);
    buf_C2F = (char*) malloc(bytes_C2F_per_query);

    batch_start_time_array = (std::chrono::system_clock::time_point*) malloc(in_total_batch_num * sizeof(std::chrono::system_clock::time_point));
    batch_finish_time_array = (std::chrono::system_clock::time_point*) malloc(in_total_batch_num * sizeof(std::chrono::system_clock::time_point));
    batch_duration_ms_array = (double*) malloc(in_total_batch_num * sizeof(double));

    assert (in_num_FPGA < MAX_FPGA_NUM);
  }


  void connect_C2F() {

    for (int n = 0; n < num_FPGA; n++) {
      
      printf("Printing C2F_port from C2F thread %d\n", C2F_port[n]);

      struct sockaddr_in serv_addr;

      if ((sock_c2f[n] = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        printf("\n Socket creation error \n");
        return;
      }

      serv_addr.sin_family = AF_INET;
      serv_addr.sin_port = htons(C2F_port[n]);

      // Convert IPv4 and IPv6 addresses from text to binary form
      // if(inet_pton(AF_INET, "127.0.0.1", &serv_addr.sin_addr)<=0)
      // if(inet_pton(AF_INET, "10.1.212.153", &serv_addr.sin_addr)<=0)
      if (inet_pton(AF_INET, FPGA_IP_addr[n], &serv_addr.sin_addr) <= 0) {
        printf("\nInvalid address/ Address not supported \n");
        return;
      }

      if (connect(sock_c2f[n], (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        printf("\nConnection Failed \n");
        return;
      }
      std::cout << "C2F sock: " << sock_c2f[n] << std::endl;
    }

    printf("Start sending data.\n");
  }

  // C2F send batch header
  void send_header(char *buf_header) {
    for (int n = 0; n < num_FPGA; n++) {
      size_t sent_header_bytes = 0;
      while (sent_header_bytes < bytes_C2F_header) {
        int C2F_bytes_this_iter = (bytes_C2F_header - sent_header_bytes) < C2F_PKG_SIZE ? (bytes_C2F_header - sent_header_bytes) : C2F_PKG_SIZE;
        int C2F_bytes = send(sock_c2f[n], &buf_header[sent_header_bytes], C2F_bytes_this_iter, 0);
        sent_header_bytes += C2F_bytes;
        if (C2F_bytes == -1) {
          printf("Sending data UNSUCCESSFUL!\n");
          return;
        }
      }
    }
  }

  // C2F send a single query
  void send_query() {
    for (int n = 0; n < num_FPGA; n++) {
      size_t total_C2F_bytes = 0;
      while (total_C2F_bytes < bytes_C2F_per_query) {
        int C2F_bytes_this_iter = (bytes_C2F_per_query - total_C2F_bytes) < C2F_PKG_SIZE ? (bytes_C2F_per_query - total_C2F_bytes) : C2F_PKG_SIZE;
        int C2F_bytes = send(sock_c2f[n], &buf_C2F[total_C2F_bytes], C2F_bytes_this_iter, 0);
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
  }

  /* This method is meant to be run in a separate thread.
   * It establishes and maintains a connection to the FPGA.
   * It is resposible for sending the queries to the FPGA.
   */
  void thread_C2F() { 
      
    // wait for ready
    while(!start_F2C) {}

    connect_C2F();

    start_C2F = 1;

    ////////////////   Data transfer + Select Cells   ////////////////

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

    std::chrono::system_clock::time_point start = std::chrono::system_clock::now();

    for (int C2F_batch_id = 0; C2F_batch_id < total_batch_num + 1; C2F_batch_id++) {

      std::cout << "C2F_batch_id: " << C2F_batch_id << std::endl;

      terminate = C2F_batch_id == total_batch_num? 1 : 0; 

      memcpy(buf_header, &batch_size, 4);
      memcpy(buf_header + 4, &nprobe, 4);
      memcpy(buf_header + 8, &terminate, 4);

      send_header(buf_header);

      if (terminate) {
        break;
      }
      if (!enable_inter_batch_window) {
        sem_wait(&sem_C2F_batch_free_to_send);
      }

      batch_start_time_array[C2F_batch_id] = std::chrono::system_clock::now();
      for (int query_id = 0; query_id < batch_size; query_id++) {

        // this semaphore controls that the window size is adhered to
        // If the  semaphore currently has the value zero, then the call blocks
        //   until either it becomes possible to perform the decrement
        sem_wait(&sem_C2F_n_queries_free_to_send);


        float *current_query = query_batch + (query_id * D);
        // TODO: here, the data should be copied from somewhere -> from HNSW
        memcpy(buf_query, current_query, D * bit_byte_const::byte_float);
        // memcpy(buf_center_vectors, center_vectors, n_bytes_center_vectors);

        memcpy(buf_C2F, buf_cell_ids, n_bytes_cell_ids);
        memcpy(buf_C2F + n_bytes_cell_ids, buf_query, n_bytes_query_vector);
        memcpy(buf_C2F + n_bytes_cell_ids + n_bytes_query_vector, buf_center_vectors, n_bytes_center_vectors);
        send_query();
        sem_post(&sem_C2F_n_queries_in_flight);
        finish_C2F_query_id++;
        std::cout << "C2F finish query_id " << finish_C2F_query_id << std::endl;
      }
    }

    std::chrono::system_clock::time_point end = std::chrono::system_clock::now();
    double durationUs = (std::chrono::duration_cast<std::chrono::microseconds>(end-start).count());
    
    std::cout << "C2F side Duration (us) = " << durationUs << std::endl;
    std::cout << "C2F side QPS () = " << total_query_num / (durationUs / 1000.0 / 1000.0) << std::endl;
    std::cout << "C2F side finished." << std::endl;
    
    return; 
  } 

  void connect_F2C() {

    std::cout << "FPGA programs must be started in order (same as the input argument) " <<
      " because the F2C receive side receives connections in order " << std::endl;

    for (int n = 0; n < num_FPGA; n++) {
      
      printf("Printing F2C_port from Thread %d\n", F2C_port[n]);

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
      address.sin_port = htons(F2C_port[n]);

      // Forcefully attaching socket to the F2C_port 8080
      if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("bind failed");
        exit(EXIT_FAILURE);
      }
      if (listen(server_fd, 3) < 0) {
        perror("listen");
        exit(EXIT_FAILURE);
      }
      if ((sock_f2c[n] = accept(server_fd, (struct sockaddr *)&address, (socklen_t *)&addrlen)) < 0) {
        perror("accept");
        exit(EXIT_FAILURE);
      }
      printf("Successfully built connection.\n");
    }
  }

  void receive_answer_to_query() {

    // TODO: each FPGA's msg should has its own buffer, not a shared buf_F2C
    for (int n = 0; n < num_FPGA; n++) {
      size_t total_F2C_bytes = 0;
      while (total_F2C_bytes < bytes_F2C_per_query) {
        int F2C_bytes_this_iter = (bytes_F2C_per_query - total_F2C_bytes) < F2C_PKG_SIZE ? (bytes_F2C_per_query - total_F2C_bytes) : F2C_PKG_SIZE;
        int F2C_bytes = read(sock_f2c[n], &buf_F2C[total_F2C_bytes], F2C_bytes_this_iter);
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
  }

  void thread_F2C() { 

    connect_F2C();
    start_F2C = 1;
    while(!start_C2F) {}
    ///////////////////////////////////////
    //// START RECEIVING DATA /////////////
    ///////////////////////////////////////

    printf("Start receiving data.\n");

    ////////////////   Data transfer   ////////////////

    // Should wait until the server said all the data was sent correctly,
    // otherwise the C2Fer may send packets yet the server did not receive.

    size_t n_bytes_top_k = batch_size * bit_byte_const::byte_int;
    size_t n_bytes_vector_ids = batch_size * TOPK * (bit_byte_const::byte_long_int);
    size_t n_bytes_distances = batch_size * TOPK * (bit_byte_const::byte_float);

    int *top_k_buf = (int *)malloc(n_bytes_top_k);
    long *vector_ids_buf = (long *)malloc(n_bytes_vector_ids);
    float *distances_buf = (float *)malloc(n_bytes_distances);

    size_t byte_offset_vector_ids_buf = num_packages::AXI_size_F2C_header * bit_byte_const::byte_AXI;
    size_t byte_offset_distances_buf = (num_packages::AXI_size_F2C_header + num_packages::AXI_size_F2C_vec_ID(TOPK)) * bit_byte_const::byte_AXI;

    std::chrono::system_clock::time_point start = std::chrono::system_clock::now(); // reset after recving the first query

    // TODO: find a good way to signal to this thread that terminate signal was received and when the last batch is received.
    for (int F2C_batch_id = 0; F2C_batch_id < total_batch_num; F2C_batch_id++) {

      sem_wait(&sem_C2F_n_queries_in_flight);
      std::cout << "F2C_batch_id: " << F2C_batch_id << std::endl;

      for (int query_id = 0; query_id < batch_size; query_id++) {

        receive_answer_to_query();

        // Each query received consists of [header] [topk x ids] [topk x dists] all three padded to the next AXI packet size
        // We copy all answers into the same array such that the GPU can simply interpret it at as a 2D array
        memcpy(top_k_buf + query_id, buf_F2C, 4);
        memcpy(vector_ids_buf + query_id * TOPK, buf_F2C + byte_offset_vector_ids_buf, TOPK * (bit_byte_const::bit_long_int / 8));
        memcpy(distances_buf + query_id * TOPK, buf_F2C + byte_offset_distances_buf, TOPK * (bit_byte_const::bit_float / 8));

        finish_F2C_query_id++;
        std::cout << "F2C finish query_id " << finish_F2C_query_id << std::endl;
        // sem_post() increments (unlocks) the semaphore pointed to by sem
        sem_post(&sem_C2F_n_queries_free_to_send);
      }
      batch_finish_time_array[F2C_batch_id] = std::chrono::system_clock::now();
      if (!enable_inter_batch_window) {
        sem_post(&sem_C2F_batch_free_to_send);
      }
    }

  std::chrono::system_clock::time_point end = std::chrono::system_clock::now();
  double durationUs = (std::chrono::duration_cast<std::chrono::microseconds>(end-start).count());

  std::cout << "F2C side Duration (us) = " << durationUs << std::endl;
  std::cout << "F2C side QPS = " << total_query_num / (durationUs / 1000.0 / 1000.0) << std::endl;
  std::cout << "F2C side Finished." << std::endl;

  return;  
  }

  void start_C2F_F2C_threads() {

    // start thread with member function: https://stackoverflow.com/questions/10673585/start-thread-with-member-function
    std::thread t_F2C(&CPUCoordinator::thread_F2C, this);
    std::thread t_C2F(&CPUCoordinator::thread_C2F, this);

    t_F2C.join();
    t_C2F.join();
  }

  void calculate_latency() {

    std::vector<double> sorted_duration_ms;
    double total_ms = 0.0;

    for (int b = 0; b < total_batch_num; b++) {
      double durationUs = (std::chrono::duration_cast<std::chrono::microseconds>(
        batch_finish_time_array[b] - batch_start_time_array[b]).count());
      double durationMs = durationUs / 1000.0;
      batch_duration_ms_array[b] = durationMs;
      sorted_duration_ms.push_back(durationMs);
      total_ms += durationMs;
    }
    double ave_ms = total_ms / total_batch_num;

    std::sort(sorted_duration_ms.begin(), sorted_duration_ms.end());
    std::cout << "Latency from batches: " << std::endl;
    std::cout << "  Min (ms): " << sorted_duration_ms.front() << std::endl;
    std::cout << "  Max (ms): " << sorted_duration_ms.back() << std::endl;
    std::cout << "  Medium (ms): " << sorted_duration_ms.at(total_batch_num / 2) << std::endl;
    std::cout << "  Average (ms): " << ave_ms << std::endl;
  }
};


int main(int argc, char const *argv[]) 
{ 
  //////////     Parameter Init     //////////  
  std::cout << "Usage: " << argv[0] << " <1 num_FPGA> "
  	"<2 ~ 2 + num_FPGA - 1 FPGA_IP_addr> " 
    "<2 + num_FPGA ~ 2 + 2 * num_FPGA - 1 C2F_port> " 
    "<2 + 2 * num_FPGA ~ 2 + 3 * num_FPGA - 1 F2C_port> "
    "<2 + 3 * num_FPGA D> <3 + 3 * num_FPGA TOPK> <4 + 3 * num_FPGA batch_size> "
    "<5 + 3 * num_FPGA total_batch_num> <6 + 3 * num_FPGA nprobe> "
    "<7 + 3 * num_FPGA window_size> <8 + 3 * num_FPGA enable_inter_batch_window>" << std::endl;

  int argv_cnt = 1;
  int num_FPGA = strtol(argv[argv_cnt++], NULL, 10);
  std::cout << "num_FPGA: " << num_FPGA << std::endl;
  assert(argc == 9 + 3 * num_FPGA);
  assert(num_FPGA <= MAX_FPGA_NUM);

  const char* FPGA_IP_addr[num_FPGA];
  for (int n = 0; n < num_FPGA; n++) {
      FPGA_IP_addr[n] = argv[argv_cnt++];
      std::cout << "FPGA " << n << " IP addr: " << FPGA_IP_addr[n] << std::endl;
  }   
  // FPGA_IP_addr = "10.253.74.5"; // alveo-build-01
  // FPGA_IP_addr = "10.253.74.12"; // alveo-u250-01
  // FPGA_IP_addr = "10.253.74.16"; // alveo-u250-02
  // FPGA_IP_addr = "10.253.74.20"; // alveo-u250-03
  // FPGA_IP_addr = "10.253.74.24"; // alveo-u250-04

  unsigned int C2F_port[num_FPGA];
  for (int n = 0; n < num_FPGA; n++) {
      C2F_port[n] = strtol(argv[argv_cnt++], NULL, 10);
      std::cout << "C2F_port " << n << ": " << C2F_port[n] << std::endl;
  } 

  unsigned int F2C_port[num_FPGA];
  for (int n = 0; n < num_FPGA; n++) {
      F2C_port[n] = strtol(argv[argv_cnt++], NULL, 10);
      std::cout << "F2C_port " << n << ": " << F2C_port[n] << std::endl;
  } 
    
  size_t D = strtol(argv[argv_cnt++], NULL, 10);
  std::cout << "D: " << D << std::endl;

  size_t TOPK = strtol(argv[argv_cnt++], NULL, 10);
  std::cout << "TOPK: " << TOPK << std::endl;

  int batch_size = strtol(argv[argv_cnt++], NULL, 10);
  std::cout << "batch_size: " << batch_size << std::endl;

  int total_batch_num = strtol(argv[argv_cnt++], NULL, 10);
  std::cout << "total_batch_num: " << total_batch_num << std::endl;

  int nprobe = strtol(argv[argv_cnt++], NULL, 10);
  std::cout << "nprobe: " << nprobe << std::endl;

    
  // how many queries are allow to send ahead of receiving results
  // e.g., when window_size = 1, query 2 can be sent without receiving result 1
  // e.g., when window_size = 0, result 1 must be received before query 2 can be sent, 
  //          but might lead to a deadlock
  int window_size = strtol(argv[argv_cnt++], NULL, 10);
  std::cout << "window_size: " << window_size << std::endl;

  // 1 = high-throughput mode, allowing inter-batch pipeline
  // 0 = low latency mode, does not send data before the last batch is finished
  int enable_inter_batch_window = strtol(argv[argv_cnt++], NULL, 10);
  std::cout << "enable_inter_batch_window: " << enable_inter_batch_window << std::endl;

  CPUCoordinator cpu_coordinator(
    D,
    TOPK, 
    batch_size,
    total_batch_num,
    nprobe,
    window_size,
    enable_inter_batch_window,
    num_FPGA,
    FPGA_IP_addr,
    C2F_port,
    F2C_port);

  cpu_coordinator.start_C2F_F2C_threads();
  cpu_coordinator.calculate_latency();

  return 0; 
} 
