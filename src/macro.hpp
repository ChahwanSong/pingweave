#pragma once

#include <infiniband/verbs.h>

#include <string>

// get a absolute path of source directory
#ifndef SOURCE_DIR
#define SOURCE_DIR (".")
#endif

// directory
const std::string DIR_UPLOAD_PATH = "/../upload";
const std::string DIR_DOWNLOAD_PATH = "/../download";
const std::string DIR_LOG_PATH = "/../logs";
const std::string DIR_RESULT_PATH = "/../result";
const std::string DIR_CONFIG_PATH = "/../config";

// constants
const static int MESSAGE_SIZE = 64;  // Message size of 64B

// RDMA parameters
const static int NUM_BUFFER = 256;   // enough?
const static int TX_DEPTH = 1;       // enough?
const static int RX_DEPTH = 1;       // enough?
const static int GID_INDEX = 3;      // by default 0 (infiniband & RoCE)
const static int SERVICE_LEVEL = 0;  // by default 0 (lowest priority)
const static int BATCH_CQE = 16;     // batch size for ibv_poll_cq()
const static int GRH_SIZE = sizeof(ibv_grh);  // GRH header 40 B (see IB Spec)
const static int RDMA_TRAFFIC_CLASS = (26 << 2 | 2);       // DSCP value = 106
const static uint32_t PINGWEAVE_REMOTE_QKEY = 0x72276001;  // remote qkey

// Params for internal message queue btw threads
const static int QUEUE_SIZE = (1 << 16);     // large enough
const static int WAIT_DEQUEUE_TIME_SEC = 1;  // seconds

// Ping interval / Report interval
// PING_INTERVAL_US is the ping interval between each src-dst pair.
const static uint64_t PING_INTERVAL_RDMA_US = 1 * 1000000;  // microseconds
const static uint64_t LOAD_CONFIG_INTERVAL_MS = 10 * 1000;  // milliseconds
const static uint64_t REPORT_INTERVAL_MS = 10 * 1000;       // milliseconds
const static uint32_t CHECK_PROCESS_INTERVAL_SEC = 10;      // seconds
