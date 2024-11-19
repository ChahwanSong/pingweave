import os
import subprocess
import sys
import asyncio  # default library
import configparser  # default library
import socket

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
config.read(CONFIG_PATH)

control_host = config["controller"]["host"]
control_port = config["controller"]["port"]
interval_download_pinglist_sec = int(config["param"]["interval_download_pinglist_sec"])
interval_read_pinglist_sec = int(config["param"]["interval_read_pinglist_sec"])
logger = initialize_pinglist_logger(socket.gethostname(), "client")


async def fetch_data(ip, port, data_type: str):
    try:
        reader, writer = await asyncio.open_connection(ip, port)

        # GET request
        writer.write(f"GET /{data_type}".encode())
        await writer.drain()

        # Get response
        data = await reader.read(-1)
        logger.info(f"Received {data_type} data.")

        # Save to YAML
        try:
            yaml_file_path = os.path.join(DOWNLOAD_PATH, f"{data_type}.yaml")

            # Check the data type - dict()
            parsed_data = yaml.safe_load(data.decode())

            # Write to YAML file
            with open(yaml_file_path, "w") as yaml_file:
                yaml.dump(parsed_data, yaml_file, default_flow_style=False)

            logger.debug(f"Saved {data_type} data to {yaml_file_path}.")
        except yaml.YAMLError as e:
            logger.error(f"Failed to parse or write {data_type} as YAML: {e}")

        writer.close()
        await writer.wait_closed()

    except (ConnectionRefusedError, asyncio.TimeoutError) as e:
        logger.error(
            f"Failed to connect to the server at {ip}:{port} for {data_type}. Error: {e}"
        )
    except Exception as e:
        logger.error(f"An unexpected error occurred while fetching {data_type}: {e}")


async def send_gid_files(ip, port):
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

                    # send POST to server
                    data_to_send = (
                        f"POST /address\n{ip_address}\n{gid}\n{lid}\n{qpn}\n{times}"
                    )
                    try:
                        reader, writer = await asyncio.open_connection(ip, port)
                        writer.write(data_to_send.encode())
                        await writer.drain()
                        logger.debug(
                            f"Sent POST address for {ip_address} to the server."
                        )
                        writer.close()
                        await writer.wait_closed()
                    except Exception as e:
                        logger.error(
                            f"Failed to send POST gid/lid address for {ip_address}: {e}"
                        )


async def main():
    while True:
        await asyncio.gather(
            send_gid_files(control_host, control_port),
            fetch_data(control_host, control_port, "pinglist"),
            fetch_data(control_host, control_port, "address_store"),
        )
        await asyncio.sleep(
            interval_download_pinglist_sec
        )  # sleep to prevent high CPU usage


if __name__ == "__main__":
    asyncio.run(main())
