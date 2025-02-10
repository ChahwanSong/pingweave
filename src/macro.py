import os

# Configuration paths
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
CONFIG_PATH = os.path.join(SCRIPT_DIR, "../config/pingweave.ini")
PINGLIST_PATH = os.path.join(SCRIPT_DIR, "../config/pinglist.yaml")

LOG_DIR = os.path.join(SCRIPT_DIR, "../logs")
UPLOAD_PATH = os.path.join(SCRIPT_DIR, "../upload")
DOWNLOAD_PATH = os.path.join(SCRIPT_DIR, "../download")
SUMMARY_DIR = os.path.join(SCRIPT_DIR, "../summary")
WEBSERVER_DIR = os.path.join(SCRIPT_DIR, "../webserver")
BOOTSTRAP_DIR = os.path.join(SCRIPT_DIR, "../libs/bootstrap")
BIN_DIR = os.path.join(SCRIPT_DIR, "../bin")
HTML_DIR = os.path.join(SCRIPT_DIR, "../html")
IMAGE_DIR = os.path.join(SCRIPT_DIR, "../image")
SHMEM_DIR = os.path.abspath("/dev/shm")

# filter out in plotting if a data is too old
INTERVAL_PLOTTER_FILTER_OLD_DATA_SEC = 120

# Redis pub/sub topic
REDIS_TOPIC_PINGWEAVE_RESULT = "redis_pingweave"

# Redis pub/sub thresholds
REDIS_THRES_AVG_NETWORK_LAT_NS = {
    "roce": 0 * 1000,
    "ib": 0 * 1000,
    "tcp": 0 * 1000,
    "udp": 0 * 1000,
}
REDIS_THRES_N_FAILURE = 0

# Redis socket path
REDIS_SOCKET_PATH = "/var/run/redis/redis-server.sock"

# Match C++ constants for Inter-Process Communication (IPC)
IPC_MESSAGE_SIZE = 2097152  # 2MB supports > 20K lines
IPC_BUFFER_SIZE = 64  # 64 messages

# Target protocols
TARGET_PROTOCOLS = list(REDIS_THRES_AVG_NETWORK_LAT_NS.keys())

