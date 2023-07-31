"""
This scripts executes the CPU and FPGA programs by reading the config file and automatically generate the command line arguments.

Example (one CPU and one FPGA)):
	In terminal 1:
		python launch_CPU_and_FPGA.py --config_fname ./config/local_network_test_1_FPGA.yaml --mode CPU
	In terminal 2:
		python launch_CPU_and_FPGA.py --config_fname ./config/local_network_test_1_FPGA.yaml --mode FPGA --fpga_id 0

Example (one CPU and two FPGA)):
	In terminal 1:
		python launch_CPU_and_FPGA.py --config_fname ./config/local_network_test_2_FPGA.yaml --mode CPU
	In terminal 2 ~ 3:
		python launch_CPU_and_FPGA.py --config_fname ./config/local_network_test_2_FPGA.yaml --mode FPGA --fpga_id 0
		python launch_CPU_and_FPGA.py --config_fname ./config/local_network_test_2_FPGA.yaml --mode FPGA --fpga_id 1


Example (one CPU and four FPGA)):
	In terminal 1:
		python launch_CPU_and_FPGA.py --config_fname ./config/local_network_test_4_FPGA.yaml --mode CPU
	In terminal 2 ~ 5:
		python launch_CPU_and_FPGA.py --config_fname ./config/local_network_test_4_FPGA.yaml --mode FPGA --fpga_id 0
		python launch_CPU_and_FPGA.py --config_fname ./config/local_network_test_4_FPGA.yaml --mode FPGA --fpga_id 1
		python launch_CPU_and_FPGA.py --config_fname ./config/local_network_test_4_FPGA.yaml --mode FPGA --fpga_id 2
		python launch_CPU_and_FPGA.py --config_fname ./config/local_network_test_4_FPGA.yaml --mode FPGA --fpga_id 3
"""

import argparse 
import json
import os
import yaml

import numpy as np

from helper import save_obj, load_obj

parser = argparse.ArgumentParser()
parser.add_argument('--config_fname', type=str, default='./config/local_network_test_1_FPGA.yaml')
parser.add_argument('--mode', type=str, help='CPU or FPGA')

# if run in the CPU mode
parser.add_argument('--cpu_exe_dir', type=str, default='./CPU_to_FPGA', help="the CPP exe file")

# if run in the FPGA simulator mode
parser.add_argument('--fpga_simulator_exe_dir', type=str, default='./FPGA_simulator', help="the FPGA simulator exe file")
parser.add_argument('--fpga_id', type=int, default=0, help="the FPGA ID (should start from 0, 1, 2, ...)")

args = parser.parse_args()
config_fname = args.config_fname
mode = args.mode
if mode == 'CPU':
	cpu_exe_dir = args.cpu_exe_dir
elif mode == 'FPGA':
	fpga_simulator_exe_dir = args.fpga_simulator_exe_dir
	fpga_id = args.fpga_id
	
def dict_to_string(data):
    if isinstance(data, list):
        return [dict_to_string(x) for x in data]
    elif isinstance(data, dict):
        return {dict_to_string(key): dict_to_string(val) for key, val in data.items()}
    else:
        return str(data)

def get_board_ID(FPGA_IP_addr):
	"""
	Given the IP address, return the boardNum argument passed to the FPGA network stack
	#   alveo-u250-01: 10.253.74.12
	#   alveo-u250-02: 10.253.74.16
	#   alveo-u250-03: 10.253.74.20
	#   alveo-u250-04: 10.253.74.24
	#   alveo-u250-05: 10.253.74.28
	#   alveo-u250-06: 10.253.74.40
	"""
	if FPGA_IP_addr == '10.253.74.12':
		return '1'
	elif FPGA_IP_addr == '10.253.74.16':
		return '2'
	elif FPGA_IP_addr == '10.253.74.20':
		return '3'
	elif FPGA_IP_addr == '10.253.74.24':
		return '4'
	elif FPGA_IP_addr == '10.253.74.28':
		return '5'
	elif FPGA_IP_addr == '10.253.74.40':
		return '6'
	else:
		return None


config_file = open(args.config_fname, "r")
config_dict = yaml.safe_load(config_file)
config_dict = dict_to_string(config_dict)
print(config_dict)
assert int(config_dict['num_FPGA']) == len(config_dict['FPGA_IP_addr'])
assert int(config_dict['num_FPGA']) == len(config_dict['C2F_port'])
assert int(config_dict['num_FPGA']) == len(config_dict['F2C_port'])

if mode == 'CPU':
	"""
  std::cout << "Usage: " << argv[0] << " <1 num_FPGA> "
  	"<2 ~ 2 + num_FPGA - 1 FPGA_IP_addr> " 
    "<2 + num_FPGA ~ 2 + 2 * num_FPGA - 1 C2F_port> " 
    "<2 + 2 * num_FPGA ~ 2 + 3 * num_FPGA - 1 F2C_port> "
    "<2 + 3 * num_FPGA D> <3 + 3 * num_FPGA TOPK> <4 + 3 * num_FPGA batch_size> "
    "<5 + 3 * num_FPGA total_batch_num> <6 + 3 * num_FPGA nprobe> <7 + 3 * num_FPGA nlist>"
    "<8 + 3 * num_FPGA query_window_size> <9 + 3 * num_FPGA batch_window_size>" 
    "<10 + 3 * num_FPGA enable_index_scan> <11 + 3 * num_FPGA omp_threads>"<< std::endl;
	"""

	# print the FPGA commands if the FPGA is available
	for i in range(int(config_dict['num_FPGA'])):
		FPGA_IP_addr = config_dict['FPGA_IP_addr'][i]	
		if get_board_ID(FPGA_IP_addr) is not None:
			board_ID = get_board_ID(FPGA_IP_addr)
			# std::cout << "Usage: " << argv[0] << " <XCLBIN File 1> <local_FPGA_IP 2> <RxPort (C2F) 3> <TxIP (CPU IP) 4> <TxPort (F2C) 5> <FPGA_board_ID 6" << std::endl;
			print(f'FPGA {i} commands: ')
			print("./host/host build_dir.hw.xilinx_u250_gen3x16_xdma_4_1_202210_1/network.xclbin "
				f" {FPGA_IP_addr} {config_dict['C2F_port'][i]} {config_dict['CPU_IP_addr']} {config_dict['F2C_port'][i]} {board_ID}")

	# execute the CPU command
	cmd = ''
	cmd += 'taskset --cpu-list 0-{} '.format(config_dict['cpu_cores'])
	cmd += cpu_exe_dir + ' '
	cmd += config_dict['num_FPGA'] + ' '
	for i in range(int(config_dict['num_FPGA'])):
		cmd += config_dict['FPGA_IP_addr'][i] + ' '
	for i in range(int(config_dict['num_FPGA'])):
		cmd += config_dict['C2F_port'][i] + ' '
	for i in range(int(config_dict['num_FPGA'])):
		cmd += config_dict['F2C_port'][i] + ' '
	cmd += config_dict['D'] + ' '
	cmd += config_dict['TOPK'] + ' '
	cmd += config_dict['batch_size'] + ' '
	cmd += config_dict['total_batch_num'] + ' '
	cmd += config_dict['nprobe'] + ' '
	cmd += config_dict['nlist'] + ' '
	cmd += config_dict['query_window_size'] + ' '
	cmd += config_dict['batch_window_size'] + ' '
	cmd += config_dict['enable_index_scan'] + ' '
	cmd += config_dict['cpu_cores'] + ' '
	print('Executing: ', cmd)
	os.system(cmd)

	print('Loading and copying profile...')
	latency_ms_distribution = np.fromfile('profile_latency_ms_distribution.double', dtype=np.float64).reshape(-1,)
	# deep copy latency_ms_distribution
	latency_ms_distribution_sorted = np.array(latency_ms_distribution)
	latency_ms_distribution_sorted.sort()
	latency_ms_min = latency_ms_distribution_sorted[0]
	latency_ms_max = latency_ms_distribution_sorted[-1]
	latency_ms_median = latency_ms_distribution_sorted[int(len(latency_ms_distribution_sorted) / 2)]
	QPS = np.fromfile('profile_QPS.double', dtype=np.float64).reshape(-1,)[0]

	print("Loaded profile: ")
	print("latency_ms_min: ", latency_ms_min)
	print("latency_ms_max: ", latency_ms_max)
	print("latency_ms_median: ", latency_ms_median)
	print("QPS: ", QPS)
	
	config_dict['latency_ms_distribution'] = latency_ms_distribution
	config_dict['latency_ms_min'] = latency_ms_min
	config_dict['latency_ms_max'] = latency_ms_max
	config_dict['latency_ms_median'] = latency_ms_median
	config_dict['QPS'] = QPS

	fname = os.path.basename(config_fname).split('.')[0] 
	save_obj(config_dict, 'performance_pickle', fname)


elif mode == 'FPGA':
	"""
	 "Usage: " << argv[0] << " <1 Tx (CPU) CPU_IP_addr> <2 Tx F2C_port> <3 Rx C2F_port> <4 D> <5 TOPK>"
	"""
	cmd = ''
	cmd += fpga_simulator_exe_dir + ' '
	cmd += config_dict['CPU_IP_addr'] + ' '
	cmd += config_dict['F2C_port'][fpga_id] + ' '
	cmd += config_dict['C2F_port'][fpga_id] + ' '
	cmd += config_dict['D'] + ' '
	cmd += config_dict['TOPK'] + ' '
	print('Executing: ', cmd)
	os.system(cmd)