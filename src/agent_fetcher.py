#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import os
import sys
import time
import json
import socket
import random
import yaml  # python3 -m pip install pyyaml
import urllib.request  # python3 -m pip install urllib
import urllib.error

from logger import initialize_pingweave_logger
from macro import *
from common import *

logger = initialize_pingweave_logger(socket.gethostname(), "agent_fetcher", 10, False)

# Python version
python_version = sys.version_info
if python_version < (3, 6):
    logger.critical(f"Python 3.6 or higher is required. Current version: {sys.version}")
    sys.exit(1)

retry_cntr_fetch_data = {}


def fetch_data(ip: str, port: str, data_type: str):
    """
    Fetches data from the server and saves it as a YAML file.
    """
    if not os.path.exists(DOWNLOAD_PATH):
        os.makedirs(DOWNLOAD_PATH)

    global retry_cntr_fetch_data
    if data_type not in retry_cntr_fetch_data:
        retry_cntr_fetch_data[data_type] = 0

    yaml_file_path = os.path.join(DOWNLOAD_PATH, f"{data_type}.yaml")

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
                        logger.warning(
                            f"Failed to safe_load YAML file {yaml_file_path}"
                        )
                        existing_data = None

                if existing_data == parsed_data:
                    logger.debug(f"No changes detected in {data_type} data.")
                    retry_cntr_fetch_data[data_type] = 0
                    return  # 데이터가 동일하면 조기 종료

            # Write to YAML file
            with open(yaml_file_path, "w") as yaml_file:
                yaml.dump(parsed_data, yaml_file, default_flow_style=False)

            logger.info(f"Saved new data '{data_type}' to '{yaml_file_path}'.")
            retry_cntr_fetch_data[data_type] = 0
            return

    except (yaml.YAMLError, json.JSONDecodeError) as e:
        logger.error(f"Failed to parse or write {data_type} as YAML: {e}")

    except urllib.error.URLError as e:
        logger.error(
            f"Failed to connect to the server at {ip}:{port} for {data_type}. Error: {e}"
        )
    except Exception as e:
        logger.error(f"An unexpected error occurred while fetching {data_type}: {e}")

    # empty YAMl if more than 3 failures
    retry_cntr_fetch_data[data_type] += 1
    if retry_cntr_fetch_data[data_type] > 3:
        logger.error(f"Error occured 3 times. Dump an empty YAML for {data_type}.")
        try:
            with open(yaml_file_path, "w") as yaml_file:
                yaml.dump({}, yaml_file, default_flow_style=False)
        except Exception as e:
            logger.critical(f"Failed to dump an empty YAML for {data_type}.")
        retry_cntr_fetch_data[data_type] = 0


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


def agent_fetcher():
    """
    (1) upload gid
    (2) download pinglist, address_store
    """

    while True:
        # GID send
        send_gid_files(config["control_host"], config["control_port"])

        # download pinglist, address_store
        fetch_data(config["control_host"], config["control_port"], "pinglist")
        fetch_data(config["control_host"], config["control_port"], "address_store")

        # random sleep to avoid request bursts
        time.sleep(config["interval_sync_pinglist_sec"] + random.randint(0, 100) * 0.01)


def run_agent_fetcher():
    try:
        agent_fetcher()
    except KeyboardInterrupt:
        logger.info("agent_fetcher process received KeyboardInterrupt. Exiting.")
