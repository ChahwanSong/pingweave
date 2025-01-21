import os
import sys
import configparser
import time
import json
import socket
import random
import yaml  # python3 -m pip install pyyaml
import urllib.request  # python3 -m pip install urllib
import urllib.error
from logger import initialize_pingweave_logger
from macro import *

logger = initialize_pingweave_logger(socket.gethostname(), "client", 30, False)

# ConfigParser object
config = configparser.ConfigParser()

# Global variables
control_host = None
control_port = None
collect_port = None
interval_sync_pinglist_sec = None
interval_read_pinglist_sec = None

python_version = sys.version_info
if python_version < (3, 6):
    logger.critical(f"Python 3.6 or higher is required. Current version: {sys.version}")
    sys.exit(1)


def load_config_ini():
    """
    Reads the configuration file and updates global variables.
    """
    global control_host, control_port, collect_port, interval_sync_pinglist_sec, interval_read_pinglist_sec

    try:
        config.read(CONFIG_PATH)

        # Update variables
        control_host = config["controller"]["host"]
        control_port = int(config["controller"]["port_control"])
        collect_port = int(config["controller"]["port_collect"])

        interval_sync_pinglist_sec = int(config["param"]["interval_sync_pinglist_sec"])
        interval_read_pinglist_sec = int(config["param"]["interval_read_pinglist_sec"])

        logger.debug(f"Configuration reloaded successfully from {CONFIG_PATH}.")
    except Exception as e:
        logger.error(f"Error reading configuration: {e}")
        logger.error(
            "Using default parameters: interval_sync_pinglist_sec=60, interval_read_pinglist_sec=60"
        )
        interval_sync_pinglist_sec = 60
        interval_read_pinglist_sec = 60


def fetch_data(ip: str, port: str, data_type: str):
    """
    Fetches data from the server and saves it as a YAML file.
    """
    if not os.path.exists(DOWNLOAD_PATH):
        os.makedirs(DOWNLOAD_PATH)

    yaml_file_path = os.path.join(DOWNLOAD_PATH, f"{data_type}.yaml")
    is_error = False
    try:
        url = f"http://{ip}:{port}/{data_type}"
        logger.debug(f"Requesting {url}")
        request = urllib.request.Request(url)
        with urllib.request.urlopen(request, timeout=5) as response:
            data = response.read()
            logger.debug(f"Received {data_type} data.")

            # Parse JSON data
            parsed_data = json.loads(data.decode())

            # Check if file exists and compare
            if os.path.exists(yaml_file_path):
                with open(yaml_file_path, "r") as existing_file:
                    try:
                        existing_data = yaml.safe_load(existing_file)
                    except yaml.YAMLError as e:
                        logger.error(f"Failed to load YAML file {yaml_file_path}: {e}")
                        existing_data = None

                if existing_data == parsed_data:
                    logger.debug(f"No changes detected in {data_type} data.")
                    return  # Exit early if data is unchanged

            # Write to YAML file
            with open(yaml_file_path, "w") as yaml_file:
                yaml.dump(parsed_data, yaml_file, default_flow_style=False)

            logger.info(f"Saved new data '{data_type}' to '{yaml_file_path}'.")

    except (yaml.YAMLError, json.JSONDecodeError) as e:
        logger.error(f"Failed to parse or write {data_type} as YAML: {e}")
        is_error = True
    except urllib.error.URLError as e:
        logger.error(
            f"Failed to connect to the server at {ip}:{port} for {data_type}. Error: {e}"
        )
        is_error = True
    except Exception as e:
        logger.error(f"An unexpected error occurred while fetching {data_type}: {e}")
        is_error = True

    # If an error occurs, write an empty YAML file to prevent issues
    if is_error:
        logger.error(f"Error occured -> Dump an empty YAML for {data_type}.")
        with open(yaml_file_path, "w") as yaml_file:
            yaml.dump({}, yaml_file, default_flow_style=False)


def send_gid_files(ip, port):
    """
    Sends GID files to the server via HTTP POST requests.
    """
    for filename in os.listdir(UPLOAD_PATH):
        filepath = os.path.join(UPLOAD_PATH, filename)

        if (
            os.path.isfile(filepath) and filename.count(".") == 3
        ):  # Validate IP-like filenames
            try:
                with open(filepath, "r") as file:
                    lines = file.read().splitlines()

                if len(lines) != 4:
                    logger.warning(
                        f"Skipping file {filename}: Expected 4 lines, got {len(lines)}."
                    )
                    continue

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

                try:
                    data_json = json.dumps(data).encode("utf-8")
                except (TypeError, ValueError) as json_error:
                    logger.error(
                        f"Failed to encode JSON for file {filename}: {json_error}"
                    )
                    continue

                url = f"http://{ip}:{port}/address"
                request = urllib.request.Request(url, data=data_json, method="POST")
                request.add_header("Content-Type", "application/json")

                try:
                    with urllib.request.urlopen(request) as response:
                        response_data = response.read().decode("utf-8")
                        if response.status == 200:
                            logger.debug(
                                f"Successfully sent data from {filename} to the server: {response_data}"
                            )
                        else:
                            logger.warning(
                                f"Server responded with status {response.status} for {filename}: {response_data}"
                            )
                except urllib.error.HTTPError as e:
                    logger.error(
                        f"HTTPError for file {filename} ({e.code}): {e.reason}"
                    )
                except urllib.error.URLError as e:
                    logger.error(
                        f"URLError while sending data from file {filename}: {e.reason}"
                    )
                except Exception as e:
                    logger.error(
                        f"Unexpected error while sending data from file {filename}: {e}"
                    )

            except FileNotFoundError as e:
                logger.error(f"File {filename} not found: {e}")
            except IOError as e:
                logger.error(f"Failed to read file {filename}: {e}")
            except Exception as e:
                logger.error(f"Unexpected error with file {filename}: {e}")


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
        time.sleep(interval_sync_pinglist_sec + random.randint(0, 10) * 0.01)


if __name__ == "__main__":
    main()
