# Test script for a single pair (client-server model)
RDMA UD with RNIC timestamping. This is to reproduce the paper "r-pingmesh" published in SIGCOMM 2024.

## Pre-requisite
1. Client NIC must be Mellanox Connect-X supporting RoCEv2 or infiniband
2. Server NIC must be Mellanox Connect-X supporting RoCEv2 or infiniband
3. Client and Server must have a shared (mounted) folder to share a config file `config/local_server_info.yaml`.


