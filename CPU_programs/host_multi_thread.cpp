// host_multi_thread: the complete implementation for multiple FPGA
//   per FPGA, there are 2 threads, 1 for sending query, 1 for receiving results
//   and an additional thread for HNSW index search
//   the cells to scan can either be selected using HNSW or brute-force scan
//   the cells to scan is computed at query time

// Refer to https://github.com/WenqiJiang/FPGA-ANNS-with_network/blob/master/CPU_scripts/unused/network_send.c
//  "Usage: " << argv[0] << "<num_FPGA> <Tx (FPGA) IP_addr (num_FPGA))> <Tx send_port (num_FPGA)> <Rx recv_port (num_FPGA)> <SEND_RECV_GAP (0~N, == batch size - 1)> <nprobe>" 
// e.g. 1 FPGA: ./host_multi_thread 10.253.74.12 8881 5001 0 32
// e.g. 2 FPGA: ./host_multi_thread 10.253.74.12 10.253.74.16 8881 8882 5001 5002 0 32
// e.g. 4 FPGA: ./host_multi_thread 10.253.74.12 10.253.74.16 10.253.74.20 10.253.74.24 8881 8882 8883 8884 5001 5002 5003 5004 0 32

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

#include "./hnswlib/hnswlib.h"

#define D 128
#define TOPK 100

#define SEND_PKG_SIZE 1024 // 1024 
#define RECV_PKG_SIZE 4096 // 1024

#define DEBUG
// #define PAD // pad result size to 2048


template <typename T>
struct aligned_allocator
{
  using value_type = T;
  T* allocate(std::size_t num)
  {
    void* ptr = nullptr;
    if (posix_memalign(&ptr,4096,num*sizeof(T)))
      throw std::bad_alloc();
    return reinterpret_cast<T*>(ptr);
  }
  void deallocate(T* p, std::size_t num)
  {
    free(p);
  }
};

// boost::filesystem does not compile well, so implement this myself
std::string dir_concat(std::string dir1, std::string dir2) {
    if (dir1.back() != '/') {
        dir1 += '/';
    }
    return dir1 + dir2;
}

void thread_HNSW(
    int query_num, int nprobe, 
    float* query_vectors_ptr, float* vector_quantizer_ptr, 
    hnswlib::AlgorithmInterface<float>* alg_hnswlib,
    int num_FPGA, int send_recv_query_gap, 
    int* start_send, int* finish_recv_query_id, int* finish_hnsw_id,
    char* FPGA_input,
    std::chrono::system_clock::time_point* query_start_time_ptr);

void thread_send_packets(
    const char* IP_addr, unsigned int send_port, 
    int num_FPGA, int thread_id, int query_num, int nprobe, int send_bytes_per_query,
    char* FPGA_input, int* finish_hnsw_id, int* start_recv, int* start_send);

void thread_recv_packets(
    unsigned int recv_port, int num_FPGA, int thread_id, int query_num, int recv_bytes_per_query, char* out_buf,
    int* start_recv, int* finish_recv_query_id);

void thread_result_gather(
    int num_FPGA, int query_num, int*finish_recv_query_id,
    std::chrono::system_clock::time_point* query_finish_time_ptr);

int main(int argc, char const *argv[]) 
{ 
    //////////     Parameter Init     //////////
    
    std::cout << "Usage: " << argv[0] << "<num_FPGA> <Tx (FPGA) IP_addr (num_FPGA))> <Tx send_port (num_FPGA)> <Rx recv_port (num_FPGA)> <SEND_RECV_GAP (0~N, == batch size - 1)> <nprobe>" << std::endl;

    int arg_count = 1;
    unsigned int num_FPGA = 1;
    num_FPGA = strtol(argv[arg_count++], NULL, 10);
    
    const char* IP_addr[num_FPGA];
    for (int i = 0; i < num_FPGA; i++) {
        IP_addr[i] = argv[arg_count++];
    }
    // IP_addr = "10.253.74.5"; // alveo-build-01
    // IP_addr = "10.253.74.16"; // alveo-u250-02
    // IP_addr = "10.253.74.20"; // alveo-u250-03
    // IP_addr = "10.253.74.24"; // alveo-u250-04

    unsigned int send_port[num_FPGA] = { 8888 };
    for (int i = 0; i < num_FPGA; i++) {
        send_port[i] = strtol(argv[arg_count++], NULL, 10);
    } 

    unsigned int recv_port[num_FPGA] = { 5001 };
    for (int i = 0; i < num_FPGA; i++) {
        recv_port[i] = strtol(argv[arg_count++], NULL, 10);
    } 
    
    // how many queries are allow to send ahead of receiving results
    // e.g., when send_recv_query_gap = 1, query 2 can be sent without receiving result 1
    // e.g., when send_recv_query_gap = 0, result 1 must be received before query 2 can be sent
    int send_recv_query_gap = 0;
    send_recv_query_gap = strtol(argv[arg_count++], NULL, 10);

    size_t nprobe = 1;
    nprobe = strtol(argv[arg_count++], NULL, 10);

    std::string db_name = "SIFT1000M"; // SIFT100M or SIFT1000M
    std::cout << "DB name: " << db_name << std::endl;
    
    std::string index_scan = "hnsw"; // hnsw or brute-force
    // std::string index_scan = "brute-force"; // hnsw or brute-force
    std::cout << "Index scan: " << index_scan << std::endl;

    size_t query_num = 10000;
    size_t nlist = 32768;

    assert (nprobe <= nlist);

    // in runtime (should from network) in 512-bit packet
    size_t size_header = 1;
    size_t size_cell_IDs = nprobe * 4 % 64 == 0? nprobe * 4 / 64: nprobe * 4 / 64 + 1;
    size_t size_query_vector = D * 4 % 64 == 0? D * 4 / 64: D * 4 / 64 + 1; 
    size_t size_center_vector = D * 4 % 64 == 0? D * 4 / 64: D * 4 / 64 + 1; 

    size_t FPGA_input_bytes = query_num * 64 * (size_header + size_cell_IDs + size_query_vector + nprobe * size_center_vector);
    int send_bytes_per_query = 64 * (size_header + size_cell_IDs + size_query_vector + nprobe * size_center_vector);
    std::cout << "send_bytes_per_query: " << send_bytes_per_query << std::endl;
    std::vector<char ,aligned_allocator<char >> FPGA_input(FPGA_input_bytes);

    // out
    // 128 is a random padding for network headers
    // header = 1 pkt
    size_t size_results_vec_ID = TOPK * 64 % 512 == 0?
        TOPK * 64 / 512 : TOPK * 64 / 512 + 1;
    size_t size_results_dist = TOPK * 32 % 512 == 0?
        TOPK * 32 / 512 : TOPK * 32 / 512 + 1;
    size_t size_results = 1 + size_results_vec_ID + size_results_dist; // in 512-bit packet
#ifdef PAD
    size_t size_results_padded = 32;
    size_results = size_results_padded;
#endif
    size_t out_bytes = num_FPGA * query_num * 64 * size_results;
    int recv_bytes_per_query = 64 * size_results;

    std::cout << "recv_bytes_per_query: " << recv_bytes_per_query << std::endl;

    //////////     Data loading / computing Part     //////////

    std::string data_dir_prefix;
    if (db_name == "SIFT100M") {
        data_dir_prefix = "/mnt/scratch/wenqi/Faiss_Enzian_U250_index/SIFT100M_IVF32768,PQ32";
    } else if (db_name == "SIFT1000M") {
        data_dir_prefix = "/mnt/scratch/wenqi/Faiss_Enzian_U250_index/SIFT1000M_IVF32768,PQ32";
    }
    std::string gnd_dir = "/mnt/scratch/wenqi/Faiss_experiments/bigann/gnd/";

    ///////////     get data size from disk     //////////
    
    // info used to Select Cells to Scan
    std::string query_vectors_dir_suffix("query_vectors_float32_10000_128_raw");
    std::string query_vectors_dir = dir_concat(data_dir_prefix, query_vectors_dir_suffix);
    std::ifstream query_vectors_fstream(
        query_vectors_dir, 
        std::ios::in | std::ios::binary);
    query_vectors_fstream.seekg(0, query_vectors_fstream.end);
    size_t query_vectors_size =  query_vectors_fstream.tellg();
    if (!query_vectors_size) std::cout << "query_vectors_size is 0!";
    query_vectors_fstream.seekg(0, query_vectors_fstream.beg);
    
    std::string vector_quantizer_dir_suffix("vector_quantizer_float32_32768_128_raw");
    std::string vector_quantizer_dir = dir_concat(data_dir_prefix, vector_quantizer_dir_suffix);
    std::ifstream vector_quantizer_fstream(
        vector_quantizer_dir, 
        std::ios::in | std::ios::binary);
    vector_quantizer_fstream.seekg(0, vector_quantizer_fstream.end);
    size_t vector_quantizer_size =  vector_quantizer_fstream.tellg();
    if (!vector_quantizer_size) std::cout << "vector_quantizer_size is 0!";
    vector_quantizer_fstream.seekg(0, vector_quantizer_fstream.beg);

    // ground truth 
    std::string raw_gt_vec_ID_suffix_dir;
    if (db_name == "SIFT100M") {
        raw_gt_vec_ID_suffix_dir = "idx_100M.ivecs";
    } else if (db_name == "SIFT1000M") {
        raw_gt_vec_ID_suffix_dir = "idx_1000M.ivecs";
    }
    std::string raw_gt_vec_ID_dir = dir_concat(gnd_dir, raw_gt_vec_ID_suffix_dir);
    std::ifstream raw_gt_vec_ID_fstream(
        raw_gt_vec_ID_dir,
        std::ios::in | std::ios::binary);

    std::string raw_gt_dist_suffix_dir;
    if (db_name == "SIFT100M") {
        raw_gt_dist_suffix_dir = "dis_100M.fvecs";
    } else if (db_name == "SIFT1000M") {
        raw_gt_dist_suffix_dir = "dis_1000M.fvecs";
    }
    std::string raw_gt_dist_dir = dir_concat(gnd_dir, raw_gt_dist_suffix_dir);
    std::ifstream raw_gt_dist_fstream(
        raw_gt_dist_dir,
        std::ios::in | std::ios::binary);

    //////////     Allocate Memory     //////////
    
    // on host side, used to Select Cells to Scan 
    std::vector<float, aligned_allocator<float>> query_vectors(query_vectors_size / sizeof(float));
    std::vector<float, aligned_allocator<float>> vector_quantizer(vector_quantizer_size / sizeof(float));

    // output data
    char* out_buf = new char[out_bytes];
    memset(out_buf, 0, out_bytes);
    
    // the raw ground truth size is the same for idx_1M.ivecs, idx_10M.ivecs, idx_100M.ivecs
    // recall counts the very first nearest neighbor only
    size_t raw_gt_vec_ID_size = 10000 * 1001 * sizeof(int);
    size_t gt_vec_ID_size = 10000 * sizeof(int);
    std::vector<int, aligned_allocator<int>> raw_gt_vec_ID(raw_gt_vec_ID_size / sizeof(int), 0);
    std::vector<int, aligned_allocator<int>> gt_vec_ID(gt_vec_ID_size / sizeof(int), 0);
    
    size_t raw_gt_dist_size = 10000 * 1001 * sizeof(float);
    size_t gt_dist_size = 10000 * sizeof(float);
    std::vector<float, aligned_allocator<float>> raw_gt_dist(raw_gt_dist_size / sizeof(float), 0);
    std::vector<float, aligned_allocator<float>> gt_dist(gt_dist_size / sizeof(float), 0);

    //////////     load data from disk     //////////
    std::cout << "Loading data from disk...\n";

    // on host, used to Select Cells to Scan
    char* query_vectors_char = (char*) malloc(query_vectors_size);
    query_vectors_fstream.read(query_vectors_char, query_vectors_size);
    if (!query_vectors_fstream) {
            std::cout << "error: only " << query_vectors_fstream.gcount() << " could be read";
        exit(1);
    }
    memcpy(&query_vectors[0], query_vectors_char, query_vectors_size);
    free(query_vectors_char);
    
    char* vector_quantizer_char = (char*) malloc(vector_quantizer_size);
    vector_quantizer_fstream.read(vector_quantizer_char, vector_quantizer_size);
    if (!vector_quantizer_fstream) {
            std::cout << "error: only " << vector_quantizer_fstream.gcount() << " could be read";
        exit(1);
    }
    memcpy(&vector_quantizer[0], vector_quantizer_char, vector_quantizer_size);
    free(vector_quantizer_char);

    assert(D * nlist * sizeof(float) == vector_quantizer_size);

    // ground truth
    char* raw_gt_vec_ID_char = (char*) malloc(raw_gt_vec_ID_size);
    raw_gt_vec_ID_fstream.read(raw_gt_vec_ID_char, raw_gt_vec_ID_size);
    if (!raw_gt_vec_ID_fstream) {
        std::cout << "error: only " << raw_gt_vec_ID_fstream.gcount() << " could be read";
        exit(1);
    }
    memcpy(&raw_gt_vec_ID[0], raw_gt_vec_ID_char, raw_gt_vec_ID_size);
    free(raw_gt_vec_ID_char);

    for (int i = 0; i < 10000; i++) {
        gt_vec_ID[i] = raw_gt_vec_ID[i * 1001 + 1];
    }

    char* raw_gt_dist_char = (char*) malloc(raw_gt_dist_size);
    raw_gt_dist_fstream.read(raw_gt_dist_char, raw_gt_dist_size);
    if (!raw_gt_dist_fstream) {
        std::cout << "error: only " << raw_gt_dist_fstream.gcount() << " could be read";
        exit(1);
    }
    memcpy(&raw_gt_dist[0], raw_gt_dist_char, raw_gt_dist_size);
    free(raw_gt_dist_char);

    for (int i = 0; i < 10000; i++) {
        gt_dist[i] = raw_gt_dist[i * 1001 + 1];
    }

    // HNSWlib index
    hnswlib::AlgorithmInterface<float>* alg_hnswlib;
    hnswlib::L2Space space(D);
    if (index_scan == "brute_force") {
        std::string brute_force_index_dir = dir_concat(data_dir_prefix, "hnswlib_brute_force_index.bin");
        std::ifstream f_hnswlib(brute_force_index_dir);
        bool hnswlib_index_exists = f_hnswlib.good();
        if (hnswlib_index_exists) {
            std::cout << "brute_force Index exists, loading index..." << std::endl;
            alg_hnswlib = new hnswlib::BruteforceSearch<float>(&space, brute_force_index_dir);
        }
        else {
            std::cout << "brute_force Index does not exist, creating new index..." << std::endl;
            alg_hnswlib = new hnswlib::BruteforceSearch<float>(&space, nlist);
            std::cout << "Adding data..." << std::endl;
            for (size_t i = 0; i < nlist; ++i) {
                alg_hnswlib->addPoint(vector_quantizer.data() + D * i, i);
            }
            alg_hnswlib->saveIndex(brute_force_index_dir);
        }
    } else if (index_scan == "hnsw") {
        std::string hnsw_index_dir = dir_concat(data_dir_prefix, "hnswlib_hnsw_index.bin");
        std::ifstream f_hnswlib(hnsw_index_dir);
        bool hnswlib_index_exists = f_hnswlib.good();
        if (hnswlib_index_exists) {
            std::cout << "HNSW Index exists, loading index..." << std::endl;
            alg_hnswlib = new hnswlib::HierarchicalNSW<float>(&space, hnsw_index_dir);
        }
        else {
            std::cout << "HNSW Index does not exist, creating new index..." << std::endl;
            size_t ef_construction = 128;
            alg_hnswlib = new hnswlib::HierarchicalNSW<float>(&space, nlist, ef_construction);
            std::cout << "Adding data..." << std::endl;
            for (size_t i = 0; i < nlist; ++i) {
                alg_hnswlib->addPoint(vector_quantizer.data() + D * i, i);
            }
            alg_hnswlib->saveIndex(hnsw_index_dir);
        }
    } else {
        std::cout << "index option does not exists, either brute_force or hnsw" << std::endl;
        exit(1);
    }

    //////////     Networking Part     //////////

    // inter-thread communication by shared memory
    int start_recv[num_FPGA] = { 0 }; // turns to 1 once recv conn opens, then send thread build conn
    int start_send[num_FPGA] = { 0 }; // turns to 1 once send conn opens, then HNSW thread starts
    int finish_hnsw_id = -1; // produced by HNSW thread, consumed by send thread
    int finish_recv_query_id[num_FPGA] = { -1 }; //produced by recv threads, consumed by HNSW thread


    // profiler
    std::vector<std::chrono::system_clock::time_point> query_start_time(query_num);
    std::vector<std::chrono::system_clock::time_point> query_finish_time(query_num);


    // HNSW
    std::thread t_HNSW(
        thread_HNSW,
        query_num, nprobe, 
        query_vectors.data(), vector_quantizer.data(), alg_hnswlib,
        num_FPGA, send_recv_query_gap, 
        (int*) start_send, (int*) finish_recv_query_id, &finish_hnsw_id,
        FPGA_input.data(),
        query_start_time.data());

    // send
    std::thread t_send[4];
    for (int i = 0; i < num_FPGA; i++) {
        t_send[i] = std::thread(
            thread_send_packets, 
            IP_addr[i], send_port[i], num_FPGA, i, query_num, nprobe, send_bytes_per_query,
            FPGA_input.data(), &finish_hnsw_id, (int*) start_recv, (int*) start_send);
    }

    // recv
    std::thread t_recv[4];
    for (int i = 0; i < num_FPGA; i++) {
        t_recv[i] = std::thread(
            thread_recv_packets, 
            recv_port[i], num_FPGA, i, query_num, recv_bytes_per_query, out_buf,
            (int*) start_recv, (int*) finish_recv_query_id);
    }

    std::thread t_gather(
        thread_result_gather, num_FPGA, query_num, (int*) finish_recv_query_id, query_finish_time.data());

    // gather results
    t_HNSW.join();
    for (int i = 0; i < num_FPGA; i++) { t_send[i].join(); }
    for (int i = 0; i < num_FPGA; i++) { t_recv[i].join(); }
    t_gather.join();

    //////////     Performance evaluation    //////////

    // TCP has a slow start, thus the performance of the first 1000 queries are not counted
    int start_query_id = int(query_num * 0.1);
    std::cout << "Calculating performance skipping the first 10% of query due to the slow "
        "start of TCP/IP..." << std::endl; 

    // calculate QPS
    double durationUs = (std::chrono::duration_cast<std::chrono::microseconds>(
            query_finish_time[query_num - 1] - query_start_time[start_query_id]).count());
    std::cout << "Overall QPS = " << query_num / (durationUs / 1000.0 / 1000.0) << std::endl;


    std::vector<double> durationUs_per_query(query_num - start_query_id);
    for (int i = start_query_id; i < query_num; i++) {
        double durationUs = (std::chrono::duration_cast<std::chrono::microseconds>(
            query_finish_time[i] - query_start_time[i]).count());
        durationUs_per_query[i - start_query_id] = durationUs;
#ifdef DEBUG
        std::cout << "query " << i << " duration (us) = " << durationUs << std::endl;
#endif
    }
    std::sort(durationUs_per_query.begin(), durationUs_per_query.end());
    std::cout << "Median latency (us) = " << durationUs_per_query[int(durationUs_per_query.size() * 0.5)] << std::endl;
    std::cout << "95% tail latency (us) = " << durationUs_per_query[int(durationUs_per_query.size() * 0.95)] << std::endl;
    std::cout << "Worst latency (us) = " << durationUs_per_query[durationUs_per_query.size() - 1] << std::endl;

    //////////     Recall evaluation    //////////
    std::cout << "Comparing Results..." << std::endl;
    
    int match_count_R1_at_1 = 0;
    int match_count_R1_at_10 = 0;
    int match_count_R1_at_100 = 0;
    int match_count_R_at_1 = 0;
    int match_count_R_at_10 = 0;
    int match_count_R_at_100 = 0;

    for (int query_id = 0; query_id < query_num; query_id++) {

#ifdef DEBUG
        std::cout << "query ID: " << query_id << std::endl;
#endif


        std::vector<long> hw_result_vec_ID_partial(TOPK, 0);
        std::vector<float> hw_result_dist_partial(TOPK, 0);
        std::vector<std::pair<float, int>> hw_result_pair(TOPK);

        int start_result_vec_ID_addr = (query_id * size_results + 1) * 64;
        int start_result_dist_addr = (query_id * size_results + 1 + size_results_vec_ID) * 64;

        // Load data
        memcpy(&hw_result_vec_ID_partial[0], &out_buf[start_result_vec_ID_addr], 8 * TOPK);
        memcpy(&hw_result_dist_partial[0], &out_buf[start_result_dist_addr], 4 * TOPK);
        for (int k = 0; k < TOPK; k++) {
            hw_result_pair[k] = std::make_pair(hw_result_dist_partial[k], hw_result_vec_ID_partial[k]);
        }
        std::sort(hw_result_pair.begin(), hw_result_pair.end());
        for (int k = 0; k < TOPK; k++) {
            hw_result_dist_partial[k] = hw_result_pair[k].first;
            hw_result_vec_ID_partial[k] = hw_result_pair[k].second;
        }
        

        int start_addr_gt = query_id * 1001 + 1;

        // R1@K
        for (int k = 0; k < 1; k++) {
            if (hw_result_vec_ID_partial[k] == raw_gt_vec_ID[start_addr_gt]) {
                match_count_R1_at_1++;
                break;
            }
        } 
        for (int k = 0; k < 10; k++) {
            if (hw_result_vec_ID_partial[k] == raw_gt_vec_ID[start_addr_gt]) {
                match_count_R1_at_10++;
                break;
            }
        } 
        for (int k = 0; k < TOPK; k++) {
#ifdef DEBUG
            // std::cout << "hw: " << hw_result_vec_ID_partial[k] << " gt: " << gt_vec_ID[query_id] << 
            //     "hw dist: " << hw_result_dist_partial[k] << " gt dist: " << gt_dist[query_id] << std::endl;
#endif 
            if (hw_result_vec_ID_partial[k] == raw_gt_vec_ID[start_addr_gt]) {
                match_count_R1_at_100++;
                break;
            }
        } 

        // R@K
        std::unordered_set<size_t> gt_set;
        for (int k = 0; k < 1; k++) {
            gt_set.insert(raw_gt_vec_ID[start_addr_gt + k]);
        }
        for (int k = 0; k < 1; k++) {
            // count actually means contain here...
            // https://stackoverflow.com/questions/42532550/why-does-stdset-not-have-a-contains-member-function
            if (gt_set.count(hw_result_vec_ID_partial[k])) { 
                match_count_R_at_1++;
            }
        }
        for (int k = 0; k < 10; k++) {
            gt_set.insert(raw_gt_vec_ID[start_addr_gt + k]);
        }
        for (int k = 0; k < 10; k++) {
            // count actually means contain here...
            // https://stackoverflow.com/questions/42532550/why-does-stdset-not-have-a-contains-member-function
            if (gt_set.count(hw_result_vec_ID_partial[k])) { 
                match_count_R_at_10++;
            }
        }
        for (int k = 0; k < 100; k++) {
            gt_set.insert(raw_gt_vec_ID[start_addr_gt + k]);
        }
        for (int k = 0; k < 100; k++) {
            // count actually means contain here...
            // https://stackoverflow.com/questions/42532550/why-does-stdset-not-have-a-contains-member-function
            if (gt_set.count(hw_result_vec_ID_partial[k])) { 
                match_count_R_at_100++;
            }
        }
    }

    std::cout << "R1@1: " << float(match_count_R1_at_1) / query_num << std::endl;
    std::cout << "R1@10: " << float(match_count_R1_at_10) / query_num << std::endl;
    std::cout << "R1@100: " << float(match_count_R1_at_100) / query_num << std::endl;
    std::cout << "R@1: " << float(match_count_R_at_1) / (query_num * 1) << std::endl;
    std::cout << "R@10: " << float(match_count_R_at_10) / (query_num * 10) << std::endl;
    std::cout << "R@100: " << float(match_count_R_at_100) / (query_num * 100) << std::endl;

    return 0; 
} 



void thread_HNSW(
    int query_num, int nprobe, 
    float* query_vectors_ptr, float* vector_quantizer_ptr, 
    hnswlib::AlgorithmInterface<float>* alg_hnswlib,
    int num_FPGA, int send_recv_query_gap, 
    int* start_send, int* finish_recv_query_id, int* finish_hnsw_id,
    char* FPGA_input,
    std::chrono::system_clock::time_point* query_start_time_ptr) { 
       
    printf("Printing from HNSW Thread\n"); 

    // in runtime (should from network) in 512-bit packet
    size_t size_header = 1;
    size_t size_cell_IDs = nprobe * 4 % 64 == 0? nprobe * 4 / 64: nprobe * 4 / 64 + 1;
    size_t size_query_vector = D * 4 % 64 == 0? D * 4 / 64: D * 4 / 64 + 1; 
    size_t size_center_vector = D * 4 % 64 == 0? D * 4 / 64: D * 4 / 64 + 1; 

    size_t FPGA_input_bytes = query_num * 64 * (size_header + size_cell_IDs + size_query_vector + nprobe * size_center_vector);
    int send_bytes_per_query = 64 * (size_header + size_cell_IDs + size_query_vector + nprobe * size_center_vector);

    std::vector<std::pair <float, int>> dist_array(nprobe); // (dist, cell_ID)

    // wait for send thread to be ready
    volatile int dummy_count = 0;
    while (true) { 
        // wait until all FPGAs are ready before starting
        bool start = true;
        for (int i = 0; i < num_FPGA; i++) {
            if (start_send[i] == 0) {
                start = false;
            }
        }
        if (start) {
            break;
        } else {
            dummy_count++;
        }
    }

    printf("Start running HNSW.\n");
    ////////////////   Data transfer + Select Cells   ////////////////

    std::chrono::system_clock::time_point start = std::chrono::system_clock::now();

    for (int query_id = 0; query_id < query_num; query_id++) {

        std::cout << "HNSW query_id " << query_id << std::endl;

        *(query_start_time_ptr + query_id) = std::chrono::system_clock::now();

        // select cells to scan
        size_t start_addr_FPGA_input_per_query = query_id * (size_header + size_cell_IDs + size_query_vector + nprobe * size_center_vector) * 64;
        size_t start_addr_FPGA_input_cell_ID = start_addr_FPGA_input_per_query + size_header * 64;
        size_t start_addr_FPGA_input_query_vector = start_addr_FPGA_input_per_query + (size_header + size_cell_IDs) * 64;
        size_t start_addr_FPGA_input_center_vector = start_addr_FPGA_input_per_query + (size_header + size_cell_IDs + size_query_vector) * 64;

        void* p = query_vectors_ptr + query_id * D;
        // searchKNN return type: std::priority_queue<std::pair<dist_t, labeltype >>
        auto gd = alg_hnswlib->searchKnn(p, nprobe);
        assert(gd.size() == nprobe);
        int cnt = 0;
        while (!gd.empty()) {
            dist_array[cnt] = std::make_pair(gd.top().first, int(gd.top().second));
            gd.pop();
            cnt++;
        }

        // write cell ID
        for (size_t nprobe_id = 0; nprobe_id < nprobe; nprobe_id++) {
            memcpy(&FPGA_input[start_addr_FPGA_input_cell_ID + nprobe_id * sizeof(int)], &dist_array[nprobe_id].second, sizeof(int));
#ifdef DEBUG
            // std::cout << "dist: " << dist_array[nprobe_id].first << " cell ID: " << dist_array[nprobe_id].second << "\n";
#endif
        } 

        // write query vector
	    memcpy(&FPGA_input[start_addr_FPGA_input_query_vector], query_vectors_ptr + query_id * D, D * sizeof(float));

        // write center vectors      
        for (size_t nprobe_id = 0; nprobe_id < nprobe; nprobe_id++) {

            size_t cell_ID = dist_array[nprobe_id].second;
	        memcpy(&FPGA_input[start_addr_FPGA_input_center_vector + nprobe_id * D * sizeof(float)], 
                   vector_quantizer_ptr + cell_ID * D, D * sizeof(float));
        }        

        *finish_hnsw_id = query_id;

        ///// Wenqi: send several queries in advance
        volatile int dummy_count = 0;
        while (true) { 
            // wait until all FPGAs are ready before starting
            bool start = true;
            for (int i = 0; i < num_FPGA; i++) {
                if (finish_recv_query_id[i] < query_id - send_recv_query_gap) {
                    start = false;
                }
            }
            if (start) {
                break;
            } else {
                dummy_count++;
            }
        }
    }

    std::chrono::system_clock::time_point end = std::chrono::system_clock::now();
    double durationUs = (std::chrono::duration_cast<std::chrono::microseconds>(end-start).count());
    
    std::cout << "HNSW side Duration (us) = " << durationUs << std::endl;
    std::cout << "HNSW side QPS = " << query_num / (durationUs / 1000.0 / 1000.0) << std::endl;

    std::cout << "HNSW side finished." << std::endl;
    return; 
} 

void thread_send_packets(
    const char* IP_addr, unsigned int send_port, 
    int num_FPGA, int thread_id, int query_num, int nprobe, int send_bytes_per_query,
    char* FPGA_input, int* finish_hnsw_id, int* start_recv, int* start_send) { 

    printf("Printing send_port %d from Thread %d\n", send_port, thread_id); 

    // wait for recv thread to be ready
    volatile int dummy_count = 0;
    while (true) { 
        // wait until all FPGAs are ready before starting
        bool start = true;
        for (int i = 0; i < num_FPGA; i++) {
            if (start_recv[i] == 0) {
                start = false;
            }
        }
        if (start) {
            break;
        } else {
            dummy_count++;
        }
    } 
    
    int sock = 0; 
    struct sockaddr_in serv_addr; 

    if ((sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) 
    { 
        printf("\n Socket creation error \n"); 
        return; 
    } 
   
    serv_addr.sin_family = AF_INET; 
    serv_addr.sin_port = htons(send_port); 
       
    // Convert IPv4 and IPv6 addresses from text to binary form 
    //if(inet_pton(AF_INET, "127.0.0.1", &serv_addr.sin_addr)<=0)  
    //if(inet_pton(AF_INET, "10.1.212.153", &serv_addr.sin_addr)<=0)  
    if(inet_pton(AF_INET, IP_addr, &serv_addr.sin_addr)<=0)  
    { 
        printf("\nInvalid address/ Address not supported \n"); 
        return; 
    } 
   
    if (connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr))<0) 
    { 
        printf("\nConnection Failed \n"); 
        return; 
    } 

    printf("Start sending data send_port %d from Thread %d\n", send_port, thread_id); 
    ////////////////   Data transfer   ////////////////

    std::chrono::system_clock::time_point start = std::chrono::system_clock::now();

    for (int query_id = 0; query_id < query_num; query_id++) {

        volatile int dummy_count = 0;
        // wait for HNSW thread
        while(*finish_hnsw_id < query_id) {  
            dummy_count++;
        }

        std::cout << "send query_id " << query_id << std::endl;

        // send data
        int total_sent_bytes = 0;

        while (total_sent_bytes < send_bytes_per_query) {
            int send_bytes_this_iter = (send_bytes_per_query - total_sent_bytes) < SEND_PKG_SIZE? (send_bytes_per_query - total_sent_bytes) : SEND_PKG_SIZE;
            int sent_bytes = send(sock, &FPGA_input[query_id * send_bytes_per_query + total_sent_bytes], send_bytes_this_iter, 0);
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

        if (total_sent_bytes != send_bytes_per_query) {
            printf("Sending error, sending more bytes than a block\n");
        }
    }

    std::chrono::system_clock::time_point end = std::chrono::system_clock::now();
    double durationUs = (std::chrono::duration_cast<std::chrono::microseconds>(end-start).count());
    
    std::cout << "Send side thread " << thread_id << " Duration (us) = " << durationUs << std::endl;
    std::cout << "Send side " << thread_id << " QPS = " << query_num / (durationUs / 1000.0 / 1000.0) << std::endl;

    std::cout << "Send side " << thread_id << " finished." << std::endl;
    return; 
} 


void thread_recv_packets(
    unsigned int recv_port, int num_FPGA, int thread_id, int query_num, int recv_bytes_per_query, char* out_buf,
    int* start_recv, int* finish_recv_query_id) { 

    printf("Printing recv_port %d from Thread %d\n", recv_port, thread_id); 

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
    printf("Successfully built connectionrecv_port %d from Thread %d\n", recv_port, thread_id); 
    start_recv[thread_id] = 1; // set shared register


    printf("Start receiving data recv_port %d from Thread %d\n", recv_port, thread_id); 
    ////////////////   Data transfer   ////////////////


    // Should wait until the server said all the data was sent correctly,
    // otherwise the sender may send packets yet the server did not receive.

    std::chrono::system_clock::time_point start = std::chrono::system_clock::now(); // reset after recving the first query

    for (int query_id = 0; query_id < query_num; query_id++) {

        std::cout << "recv query_id " << query_id << std::endl;
        int total_recv_bytes = 0;
        while (total_recv_bytes < recv_bytes_per_query) {
            int recv_bytes_this_iter = (recv_bytes_per_query - total_recv_bytes) < RECV_PKG_SIZE? (recv_bytes_per_query - total_recv_bytes) : RECV_PKG_SIZE;
            int recv_bytes = read(sock, out_buf + (num_FPGA * query_id + thread_id) * recv_bytes_per_query + total_recv_bytes, recv_bytes_this_iter);
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
//             if (recv_bytes > 0) {
//                 // set shared register as soon as the first packet of the results is received
//                 *finish_recv_query_id = query_id; 
// #ifdef DEBUG
//                 std::cout << "set finish_recv_query_id: " << query_id  << std::endl;
// #endif
//             }
        }
        // set shared register as soon as the first packet of the results is received
        finish_recv_query_id[thread_id] = query_id; 

        if (total_recv_bytes != recv_bytes_per_query) {
            printf("Receiving error, receiving more bytes than a block\n");
        }
        if (query_id == 0) {
            // start counting when the first recv finished
            start = std::chrono::system_clock::now();
        }
    }

    std::chrono::system_clock::time_point end = std::chrono::system_clock::now();
    double durationUs = (std::chrono::duration_cast<std::chrono::microseconds>(end-start).count());

    std::cout << "Recv side " << thread_id << " Duration (us) = " << durationUs << std::endl;
    if (query_num > 1) {
        std::cout << "Recv side " << thread_id << " QPS = " << (query_num - 1) / (durationUs / 1000.0 / 1000.0) << std::endl;
    } 
    std::cout << "Recv side " << thread_id << " Finished." << std::endl;

    return; 
} 

void thread_result_gather(
    int num_FPGA, int query_num, int*finish_recv_query_id,
    std::chrono::system_clock::time_point* query_finish_time_ptr) {
    // gather the results per thread
    // Save the result recv time


    for (int query_id = 0; query_id < query_num; query_id++) {

        volatile int dummy_count = 0;
        while (true) { 
            // wait until all FPGAs are ready before starting
            bool start = true;
            for (int i = 0; i < num_FPGA; i++) {
                if (finish_recv_query_id[i] < query_id) {
                    start = false;
                }
            }
            if (start) {
                break;
            } else {
                dummy_count++;
            }
        }

        *(query_finish_time_ptr + query_id) = std::chrono::system_clock::now();
    }
}