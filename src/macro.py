import os

# Configuration paths
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
CONFIG_PATH = os.path.join(SCRIPT_DIR, "../config/pingweave.ini")
PINGLIST_PATH = os.path.join(SCRIPT_DIR, "../config/pinglist.yaml")

LOG_DIR = os.path.join(SCRIPT_DIR, "../logs")
UPLOAD_PATH = os.path.join(SCRIPT_DIR, "../upload")
DOWNLOAD_PATH = os.path.join(SCRIPT_DIR, "../download")
HTML_DIR = os.path.join(SCRIPT_DIR, "../html")
WEBSERVER_DIR = os.path.join(SCRIPT_DIR, "../webserver")


# interval to check if the control interface is not active, when if the control IP is in this node
INTERVAL_INTERFACE_ACTIVE_SEC = 60

# filter out in plotting if a data is too old
INTERVAL_PLOTTER_FILTER_OLD_DATA_SEC = 60 