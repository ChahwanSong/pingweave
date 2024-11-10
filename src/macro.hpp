#pragma once

#include <infiniband/verbs.h>

#include <string>

// constants
const static int MESSAGE_SIZE = 64;           // Message size of 64 B
const static int GRH_SIZE = sizeof(ibv_grh);  // GRH header 40 B (see IB Spec)
const static uint64_t PING_ID_INIT = 1000000000;  // start id

// RDMA parameters
const static int TX_DEPTH = 16;      // only 1 SEND to have data consistency
const static int RX_DEPTH = 32;      // enough?
const static int GID_INDEX = 0;      // by default 0 (infiniband & RoCE)
const static int SERVICE_LEVEL = 3;  // by default 3 (TC = 3)

// Params for IPC (inter-processor communication)
const static int BATCH_SIZE = 16;               // Process messages in batches
const static int BUFFER_SIZE = BATCH_SIZE + 1;  // Message queue's buffer size
const static int BATCH_FLUSH_TIMEOUT_MS = 100;  // Timeout in milliseconds
const static int CONSUMER_WAIT_TIMEOUT_MS = 1;  // Timeout in milliseconds
const std::string PREFIX_SHMEM_NAME = "/pingweave_";  // Name of shared memory

// Params for internal message queue btw threads
const static int QUEUE_SIZE = 1000;

// Ping interval
const static uint64_t PING_INTERVAL_US = 1000;  // microseconds

// directory
const std::string DIR_UPLOAD_PATH = "/../upload";
const std::string DIR_DOWNLOAD_PATH = "/../download/";
const std::string DIR_LOG_PATH = "/../logs/";
const std::string DIR_RESULT_PATH = "/../result/";
