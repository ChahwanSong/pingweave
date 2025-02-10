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
INTERVAL_PLOTTER_FILTER_OLD_DATA_SEC = 60

# Match C++ constants for Inter-Process Communication (IPC)
IPC_MESSAGE_SIZE = 2097152  # 2MB supports > 20K lines
IPC_BUFFER_SIZE = 64  # 64 messages

# Target protocols
TARGET_PROTOCOLS = ["tcp", "udp", "roce", "ib"]
