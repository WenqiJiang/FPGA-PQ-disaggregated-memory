"""
This scripts executes the CPU and FPGA programs by reading the config file and automatically generate the command line arguments.

Example (one CPU and one FPGA)):
	In terminal 1:
		python launch_CPU_and_FPGA.py --config_fname ./config/local_network_test_1_FPGA.json --mode CPU
	In terminal 2:
		python launch_CPU_and_FPGA.py --config_fname ./config/local_network_test_1_FPGA.json --mode FPGA --fpga_id 0

Example (one CPU and two FPGA)):
	In terminal 1:
		python launch_CPU_and_FPGA.py --config_fname ./config/local_network_test_2_FPGA.json --mode CPU
	In terminal 2 ~ 3:
		python launch_CPU_and_FPGA.py --config_fname ./config/local_network_test_2_FPGA.json --mode FPGA --fpga_id 0
		python launch_CPU_and_FPGA.py --config_fname ./config/local_network_test_2_FPGA.json --mode FPGA --fpga_id 1


Example (one CPU and four FPGA)):
	In terminal 1:
		python launch_CPU_and_FPGA.py --config_fname ./config/local_network_test_4_FPGA.json --mode CPU
	In terminal 2 ~ 5:
		python launch_CPU_and_FPGA.py --config_fname ./config/local_network_test_4_FPGA.json --mode FPGA --fpga_id 0
		python launch_CPU_and_FPGA.py --config_fname ./config/local_network_test_4_FPGA.json --mode FPGA --fpga_id 1
		python launch_CPU_and_FPGA.py --config_fname ./config/local_network_test_4_FPGA.json --mode FPGA --fpga_id 2
		python launch_CPU_and_FPGA.py --config_fname ./config/local_network_test_4_FPGA.json --mode FPGA --fpga_id 3
"""

import argparse 
import json
import os

parser = argparse.ArgumentParser()
parser.add_argument('--config_fname', type=str, default='./config/local_network_test_1_FPGA.json')
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

with open(config_fname, 'r') as f:
	json_dict = json.load(f)
	
def dict_to_string(data):
    if isinstance(data, list):
        return [dict_to_string(x) for x in data]
    elif isinstance(data, dict):
        return {dict_to_string(key): dict_to_string(val) for key, val in data.items()}
    else:
        return str(data)

json_dict = dict_to_string(json_dict)
print(json_dict)

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
	cmd = ''
	cmd += 'taskset --cpu-list 0-{} '.format(json_dict['cpu_cores'])
	cmd += cpu_exe_dir + ' '
	cmd += json_dict['num_FPGA'] + ' '
	for i in range(int(json_dict['num_FPGA'])):
		cmd += json_dict['FPGA_IP_addr'][i] + ' '
	for i in range(int(json_dict['num_FPGA'])):
		cmd += json_dict['C2F_port'][i] + ' '
	for i in range(int(json_dict['num_FPGA'])):
		cmd += json_dict['F2C_port'][i] + ' '
	cmd += json_dict['D'] + ' '
	cmd += json_dict['TOPK'] + ' '
	cmd += json_dict['batch_size'] + ' '
	cmd += json_dict['total_batch_num'] + ' '
	cmd += json_dict['nprobe'] + ' '
	cmd += json_dict['nlist'] + ' '
	cmd += json_dict['query_window_size'] + ' '
	cmd += json_dict['batch_window_size'] + ' '
	cmd += json_dict['enable_index_scan'] + ' '
	cmd += json_dict['cpu_cores'] + ' '
	print('Executing: ', cmd)
	os.system(cmd)

elif mode == 'FPGA':
	"""
	 "Usage: " << argv[0] << " <1 Tx (CPU) CPU_IP_addr> <2 Tx F2C_port> <3 Rx C2F_port> <4 D> <5 TOPK>"
	"""
	cmd = ''
	cmd += fpga_simulator_exe_dir + ' '
	cmd += json_dict['CPU_IP_addr'] + ' '
	cmd += json_dict['F2C_port'][fpga_id] + ' '
	cmd += json_dict['C2F_port'][fpga_id] + ' '
	cmd += json_dict['D'] + ' '
	cmd += json_dict['TOPK'] + ' '
	print('Executing: ', cmd)
	os.system(cmd)