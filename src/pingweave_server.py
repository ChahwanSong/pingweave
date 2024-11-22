import os
import sys
import subprocess
import configparser
import asyncio
import time
import copy
import socket
import psutil
import multiprocessing
import json
import signal

from logger import initialize_pingweave_logger
import yaml  # python3 -m pip install pyyaml
from aiohttp import web  # requires python >= 3.7

logger = initialize_pingweave_logger(socket.gethostname(), "server")

# Absolute paths of this script and configuration files
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
CONFIG_PATH = os.path.join(SCRIPT_DIR, "../config/pingweave.ini")  # for all
PINGLIST_PATH = os.path.join(SCRIPT_DIR, "../config/pinglist.yaml")  # for server
UPLOAD_PATH = os.path.join(SCRIPT_DIR, "../upload")  # for client
DOWNLOAD_PATH = os.path.join(SCRIPT_DIR, "../download")  # for client

# Variables to save pinglist
pinglist_in_memory = {}
address_store = {}  # ip -> (ip, gid, lid, qpn)
pinglist_lock = asyncio.Lock()
address_store_lock = asyncio.Lock()

# ConfigParser object
config = configparser.ConfigParser()

# Global variables
control_host = None
control_port = None
collect_port = None
interval_sync_pinglist_sec = None
interval_read_pinglist_sec = None

python_version = sys.version_info
if python_version < (3, 7):
    logger.critical(f"Python 3.7 or higher is required. Current version: {sys.version}")
    sys.exit(1)


def check_ip_active(target_ip):
    """
    Checks if the given IP address is active and associated with an interface that is UP.
    """
    try:
        # Get network interface info from psutil
        net_if_addrs = psutil.net_if_addrs()
        net_if_stats = psutil.net_if_stats()

        for iface, addrs in net_if_addrs.items():
            for addr in addrs:
                if addr.family == socket.AF_INET and addr.address == target_ip:
                    # Check interface status
                    if iface in net_if_stats and net_if_stats[iface].isup:
                        return True
                    else:
                        logger.error(f"Interface {iface} with IP {target_ip} is down.")
                        return False

        # No match
        logger.error(f"No active interface found with IP address {target_ip}.")
        return False

    except Exception as e:
        logger.error(f"An unexpected error occurred while checking IP: {e}")
        return False


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
            "Use default parameters - interval_sync_pinglist_sec=60, interval_read_pinglist_sec=60"
        )
        interval_sync_pinglist_sec = 60
        interval_read_pinglist_sec = 60


async def read_pinglist():
    global pinglist_in_memory

    try:
        # Use a lock to hold client request during file loading
        async with pinglist_lock:
            pinglist_in_memory.clear()
            if os.path.isfile(PINGLIST_PATH):
                with open(PINGLIST_PATH, "r") as file:
                    # YAML -> dictionary (parsing)
                    pinglist_in_memory = yaml.safe_load(file)
                    logger.debug("Pinglist is loaded successfully.")
            else:
                logger.error(f"Pinglist file not found at {PINGLIST_PATH}")
    except Exception as e:
        logger.error(f"Error loading pinglist: {e}")


async def read_pinglist_periodically():
    # Initially load a config file
    load_config_ini()

    try:
        while True:
            await read_pinglist()
            await asyncio.sleep(interval_read_pinglist_sec)
    except asyncio.CancelledError:
        logger.info("read_pinglist_periodically task was cancelled.")
    except Exception as e:
        logger.error(f"Exception in read_pinglist_periodically: {e}")


#############################################################################


async def get_pinglist(request):
    client_ip = request.remote
    async with pinglist_lock:
        response_data = pinglist_in_memory
    logger.debug(f"(SEND) pinglist.yaml to client: {client_ip}")
    return web.json_response(response_data)


async def get_address_store(request):
    client_ip = request.remote
    async with address_store_lock:
        response_data = address_store
    logger.debug(f"(SEND) address_store.yaml to client: {client_ip}")
    return web.json_response(response_data)


async def post_address(request):
    client_ip = request.remote
    try:
        data = await request.json()
        ip_address = data.get("ip_address")
        gid = data.get("gid")
        lid = data.get("lid")
        qpn = data.get("qpn")
        dtime = data.get("dtime")

        if all([ip_address, gid, lid, qpn, dtime]):
            async with address_store_lock:
                address_store[ip_address] = [ip_address, gid, int(lid), int(qpn)]
                logger.debug(
                    f"(RECV) POST from {client_ip}. Update the address store (current size: {len(address_store)})."
                )

                if len(address_store) > 10000:
                    logger.error(
                        f"Too many registered ip->(gid,lid) entries: {len(address_store)}"
                    )
                    logger.critical("Clean up the address_store. Check your config.")
                    address_store.clear()
            return web.Response(text="Address updated", status=200)
        else:
            logger.warning(f"(RECV) POST format is incorrect from {client_ip}")
            return web.Response(text="Invalid data", status=400)
    except Exception as e:
        logger.error(f"POST ADDRESS from {client_ip}: {e}")
        return web.Response(text="Internal server error", status=500)


async def pingweave_server():
    # Initially load a config file
    load_config_ini()

    try:
        while True:
            if not check_ip_active(control_host):
                logger.error(
                    f"No active iface with Control IP {control_host}. Sleep 1 minute..."
                )
                await asyncio.sleep(60)  # Sleep 60 seconds

                # Reload a config file and try
                load_config_ini()
                continue

            try:
                app = web.Application()
                app.router.add_get("/pinglist", get_pinglist)
                app.router.add_get("/address_store", get_address_store)
                app.router.add_post("/address", post_address)

                runner = web.AppRunner(app)
                await runner.setup()
                site = web.TCPSite(runner, host="0.0.0.0", port=control_port)
                await site.start()

                logger.info(
                    f"Pingweave server running on {control_host}:{control_port}"
                )

                # Start periodic pinglist reading task
                pinglist_task = asyncio.create_task(read_pinglist_periodically())

                # Keep the server running indefinitely
                await asyncio.Event().wait()

            except asyncio.CancelledError:
                logger.info("pingweave_server task was cancelled.")
                break
            except Exception as e:
                logger.error(f"Cannot start the pingweave server: {e}")
            finally:
                await runner.cleanup()
                await asyncio.sleep(10)
    except KeyboardInterrupt:
        logger.info("pingweave_server received KeyboardInterrupt. Exiting.")
    except Exception as e:
        logger.error(f"Exception in pingweave_server: {e}")


#############################################################################
async def handle_result_post(request):
    client_ip = request.remote

    try:
        # Request data as text
        raw_data = await request.text()
        logger.debug(f"Raw POST RESULT data from {client_ip}: {raw_data}")

        # Process the data (e.g., data parsing)
        print(raw_data)
        # data = raw_data.strip().split(",")  # Parse comma-separated data
        # logger.debug(f"Processed result data: {data}")

        # Return successful response
        return web.Response(text="Data processed successfully", status=200)

    except Exception as e:
        logger.error(f"Error processing POST data from {client_ip}: {e}")
        return web.Response(text="Internal server error", status=500)


async def pingweave_collector():
    # Initially load a config file
    load_config_ini()

    try:
        while True:

            if not check_ip_active(control_host):
                logger.info(
                    f"No active iface with Control IP {control_host}. Sleep 1 minute..."
                )
                await asyncio.sleep(60)  # Sleep 60 seconds

                # Reload a config file and try
                load_config_ini()
                continue

            runner = None
            try:
                app = web.Application()
                app.router.add_post("/result", handle_result_post)
                runner = web.AppRunner(app)
                await runner.setup()
                site = web.TCPSite(runner, host="0.0.0.0", port=collect_port)
                await site.start()

                logger.info(
                    f"Pingweave collector running on {control_host}:{collect_port}"
                )

                # Start periodic pinglist reading task
                pinglist_task = asyncio.create_task(read_pinglist_periodically())

                # Keep the server running indefinitely
                await asyncio.Event().wait()

            except asyncio.CancelledError:
                logger.info("pingweave_collector task was cancelled.")
                break
            except Exception as e:
                logger.error(f"Cannot start the pingweave collector: {e}")
            finally:
                if runner:
                    await runner.cleanup()
                await asyncio.sleep(10)
    except KeyboardInterrupt:
        logger.info("pingweave_collector received KeyboardInterrupt. Exiting.")
    except Exception as e:
        logger.error(f"Exception in pingweave_collector: {e}")


#############################################################################


def run_pingweave_server():
    try:
        asyncio.run(pingweave_server())
    except KeyboardInterrupt:
        logger.info("pingweave_server process received KeyboardInterrupt. Exiting.")


def run_pingweave_collector():
    try:
        asyncio.run(pingweave_collector())
    except KeyboardInterrupt:
        logger.info("pingweave_collector process received KeyboardInterrupt. Exiting.")


if __name__ == "__main__":
    try:
        process_server = multiprocessing.Process(
            target=run_pingweave_server, name="pingweave_server", daemon=True
        )
        process_collector = multiprocessing.Process(
            target=run_pingweave_collector, name="pingweave_collector", daemon=True
        )

        process_server.start()
        process_collector.start()

        while True:
            # Periodically check the process status
            if not process_server.is_alive():
                logger.warning("pingweave_server has stopped unexpectedly.")
                break
            if not process_collector.is_alive():
                logger.warning("pingweave_collector has stopped unexpectedly.")
                break
            time.sleep(1)

    except KeyboardInterrupt:
        logger.info("Main process received KeyboardInterrupt. Exiting.")
    except Exception as e:
        logger.error(f"Main loop exception: {e}. Exiting cleanly...")
    finally:
        # Terminate all processes
        if process_server.is_alive():
            process_server.terminate()
            logger.warning("Terminated pingweave_server process.")
        if process_collector.is_alive():
            process_collector.terminate()
            logger.warning("Terminated pingweave_collector process.")
