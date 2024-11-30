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
