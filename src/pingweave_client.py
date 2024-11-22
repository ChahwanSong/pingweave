import os
import subprocess
import sys
import configparser  # default library
import socket
import time
import urllib.request
import urllib.error
import json

try:
    import yaml
except ImportError:
    print("PyYAML library not found. Installing...")
    subprocess.check_call([sys.executable, "-m", "pip", "install", "pyyaml"])
    import yaml

from logger import initialize_pinglist_logger

# absolute paths of this script and pinglist.yaml
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
CONFIG_PATH = os.path.join(SCRIPT_DIR, "../config/pingweave.ini")  # for all
PINGLIST_PATH = os.path.join(SCRIPT_DIR, "../config/pinglist.yaml")  # for server
UPLOAD_PATH = os.path.join(SCRIPT_DIR, "../upload")  # for client
DOWNLOAD_PATH = os.path.join(SCRIPT_DIR, "../download")  # for client

# ConfigParser object
config = configparser.ConfigParser()

# global variables
control_host = None
control_port = None
interval_sync_pinglist_sec = None
interval_read_pinglist_sec = None

logger = initialize_pinglist_logger(socket.gethostname(), "client")

python_version = sys.version_info
if python_version < (3, 6):
    logger.error(f"Python 3.6 or higher is required. Current version: {sys.version}")
    sys.exit(1)  # 프로그램 종료


def load_config_ini():
    """
    Reads the configuration file and updates global variables.
    """
    global control_host, control_port, interval_sync_pinglist_sec, interval_read_pinglist_sec

    try:
        config.read(CONFIG_PATH)

        # 변수 업데이트
        control_host = config["controller"]["host"]
        control_port = int(config["controller"]["port"])
        interval_sync_pinglist_sec = int(config["param"]["interval_sync_pinglist_sec"])
        interval_read_pinglist_sec = int(config["param"]["interval_read_pinglist_sec"])

        logger.info(f"Configuration reloaded successfully from {CONFIG_PATH}.")
    except Exception as e:
        logger.error(f"Error reading configuration: {e}")
        logger.error(
            "Use default parameters - interval_sync_pinglist_sec=60, interval_read_pinglist_sec=60"
        )
        interval_sync_pinglist_sec = 60
        interval_read_pinglist_sec = 60


def fetch_data(ip, port, data_type):
    try:
        url = f"http://{ip}:{port}/{data_type}"
        logger.debug(f"Requesting {url}")
        request = urllib.request.Request(url)
        with urllib.request.urlopen(request) as response:
            data = response.read()
            logger.info(f"Received {data_type} data.")

            # Save to YAML
            try:
                yaml_file_path = os.path.join(DOWNLOAD_PATH, f"{data_type}.yaml")

                # Parse JSON data
                parsed_data = json.loads(data.decode())

                # Write to YAML file
                with open(yaml_file_path, "w") as yaml_file:
                    yaml.dump(parsed_data, yaml_file, default_flow_style=False)

                logger.debug(f"Saved {data_type} data to {yaml_file_path}.")
            except (yaml.YAMLError, json.JSONDecodeError) as e:
                logger.error(f"Failed to parse or write {data_type} as YAML: {e}")
    except urllib.error.URLError as e:
        logger.error(
            f"Failed to connect to the server at {ip}:{port} for {data_type}. Error: {e}"
        )
    except Exception as e:
        logger.error(f"An unexpected error occurred while fetching {data_type}: {e}")


def send_gid_files(ip, port):
    for filename in os.listdir(UPLOAD_PATH):
        filepath = os.path.join(UPLOAD_PATH, filename)

        if (
            os.path.isfile(filepath) and filename.count(".") == 3
        ):  # Check file name is IP address
            with open(filepath, "r") as file:
                lines = file.read().splitlines()
                if len(lines) == 4:
                    gid, lid, qpn, times = lines
                    ip_address = filename

                    # Prepare data to send
                    data = {
                        "ip_address": ip_address,
                        "gid": gid,
                        "lid": lid,
                        "qpn": qpn,
                        "dtime": times,
                    }
                    data_json = json.dumps(data).encode("utf-8")

                    url = f"http://{ip}:{port}/address"
                    request = urllib.request.Request(url, data=data_json, method="POST")
                    request.add_header("Content-Type", "application/json")

                    try:
                        with urllib.request.urlopen(request) as response:
                            response_data = response.read()
                            logger.debug(
                                f"Sent POST address from {ip_address} to the server."
                            )
                    except urllib.error.URLError as e:
                        logger.error(
                            f"Failed to send POST gid/lid address from {ip_address}: {e}"
                        )
                    except Exception as e:
                        logger.error(
                            f"An unexpected error occurred while sending data: {e}"
                        )


def main():
    while True:
        # Load the config file
        load_config_ini()

        # Send gid files
        send_gid_files(control_host, control_port)

        # Fetch pinglist and address_store
        fetch_data(control_host, control_port, "pinglist")
        fetch_data(control_host, control_port, "address_store")

        # Sleep to prevent high CPU usage + small delay
        time.sleep(interval_sync_pinglist_sec + 0.01)


if __name__ == "__main__":
    main()
