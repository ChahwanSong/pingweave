#pragma once

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
const static int RDMA_TRAFFIC_CLASS = 0; // (26 << 2 | 2); // DSCP value = 106
const static uint32_t PINGWEAVE_REMOTE_QKEY = 0x72276001;  // remote qkey

// Params for internal message queue btw threads
const static int QUEUE_SIZE = (1 << 16);     // large enough
const static int WAIT_DEQUEUE_TIME_SEC = 1;  // seconds

// Ping interval (for each src-dst pair) / Report interval
const static uint32_t CHECK_PROCESS_INTERVAL_SEC = 10;  // seconds
const static uint64_t LOAD_CONFIG_INTERVAL_SEC = 11;    // seconds

// Table expiry timeout
const static uint32_t PINGWEAVE_TABLE_EXPIRY_TIME_RDMA_MS = 1000;
const static uint32_t PINGWEAVE_TABLE_EXPIRY_TIME_UDP_MS = 1000;

// port that UDP server will listen
const static int PINGWEAVE_UDP_PORT_CLIENT = 33335;
const static int PINGWEAVE_UDP_PORT_SERVER = 33336; // both TX & RX

// IB HW Timestamp correction factor
const static uint64_t PINGWEAVE_TIME_CALC_MODULO = 1ULL << 32;

inline std::string get_source_directory() {
#ifndef SOURCE_DIR
    // If missed, give a current directory
    return ".";
#else
    // SOURCE_DIR will be defined in Makefile
    return SOURCE_DIR;
#endif
}

const std::string pinglist_abs_path =
    get_source_directory() + DIR_DOWNLOAD_PATH + "/pinglist.yaml";
const std::string py_client_abs_path =
    get_source_directory() + "/pingweave_client.py";
const std::string py_server_abs_path =
    get_source_directory() + "/pingweave_server.py";
