#pragma once

#include <string>

// get a absolute path of source directory
#ifndef SOURCE_DIR
#define SOURCE_DIR (".")
#endif

// function checker
#define PINGWEAVE_SUCCESS (1)
#define PINGWEAVE_FAILURE (0)
#define IS_SUCCESS(retval) ((retval) == PINGWEAVE_SUCCESS)
#define IS_FAILURE(retval) ((retval) == PINGWEAVE_FAILURE)

// get pingweave/src directory
inline std::string get_src_dir() {
#ifndef SOURCE_DIR
    // If missed, give a current directory
    return ".";
#else
    // SOURCE_DIR will be defined in Makefile
    return SOURCE_DIR;
#endif
}

// directory
const std::string DIR_UPLOAD_PATH = "/../upload";
const std::string DIR_DOWNLOAD_PATH = "/../download";
const std::string DIR_LOG_PATH = "/../logs";
const std::string DIR_RESULT_PATH = "/../result";
const std::string DIR_CONFIG_PATH = "/../config";
const std::string PINGWEAVE_INI_ABS_PATH =
    get_src_dir() + DIR_CONFIG_PATH + "/pingweave.ini";
// constants
const static int MESSAGE_SIZE = 64;                  // Message size of 64B
const static int MAX_NUM_HOSTS_IN_PINGLIST = 10000;  // Maximum of host number

// RDMA parameters
const static int NUM_BUFFER = 256;   // to avoid buffer corruption for CQE
const static int TX_DEPTH = 1;       // rdma
const static int RX_DEPTH = 1;       // rdma
const static int SERVICE_LEVEL = 0;  // by default 0 (lowest priority)
const static int BATCH_CQE = 16;     // batch size for ibv_poll_cq()
const static int SMALL_JITTERING_MICROSEC = 10;  // in event polling for cqe
const static uint32_t PINGWEAVE_REMOTE_QKEY = 0x72276001;  // remote qkey

// Params for internal message queue btw threads
const static int MSG_QUEUE_SIZE = (1 << 16);  // large enough
const static int WAIT_DEQUEUE_TIME_MS = 10;   // milliseconds

// TCP socket timeout
const static int PINGWEAVE_TCP_SOCK_TIMEOUT_SEC = 1;

// Table expiry timeout 
// IMPORTANT: MUST BE LESS THAN 2000 becuase of TIME_CALC_MODULO below)
const static uint32_t PINGWEAVE_TABLE_EXPIRY_TIME_RDMA_MS = 1000;
const static uint32_t PINGWEAVE_TABLE_EXPIRY_TIME_TCP_MS = 1000;
const static uint32_t PINGWEAVE_TABLE_EXPIRY_TIME_UDP_MS = 1000;

// port that UDP server will listen
const static int PINGWEAVE_TCP_PORT_SERVER = 33334;
const static int PINGWEAVE_UDP_PORT_CLIENT = 33335;
const static int PINGWEAVE_UDP_PORT_SERVER = 33336;

// IB HW Timestamp correction factor
const static uint64_t PINGWEAVE_TIME_CALC_MODULO = 1ULL << 32;

// IPC (inter-process communication) - The size of message and number of slots
const static int IPC_MESSAGE_SIZE = 2097152;  // 2MB
const static int IPC_BUFFER_SIZE = 256;        // 256 messages


