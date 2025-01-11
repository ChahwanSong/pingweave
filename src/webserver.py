import os
import sys
import signal
import configparser
import asyncio
import time
import socket
import psutil
import multiprocessing
import copy
from setproctitle import setproctitle

from logger import initialize_pingweave_logger
import yaml  # python3 -m pip install pyyaml
from aiohttp import web  # requires python >= 3.7
from macro import *

logger = initialize_pingweave_logger(socket.gethostname(), "webserver", 10, False)

# Variables to save pinglist
pinglist_in_memory = {}
address_store = {}  # (for RDMA) ip -> (ip, gid, lid, qpn, dtime)
address_store_checkpoint = 0
pinglist_lock = asyncio.Lock()
address_store_lock = asyncio.Lock()

# Global variables
control_host = None
control_port = None
interval_sync_pinglist_sec = None
interval_read_pinglist_sec = None


# ConfigParser object
config = configparser.ConfigParser()


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
    global control_host, control_port, interval_sync_pinglist_sec, interval_read_pinglist_sec

    try:
        config.read(CONFIG_PATH)

        # Update variables
        control_host = config["controller"]["host"]
        control_port = int(config["controller"]["port_control"])

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


async def read_pinglist():
    global pinglist_in_memory

    try:
        tmp_pinglist_in_memory = None
        if os.path.isfile(PINGLIST_PATH):
            with open(PINGLIST_PATH, "r") as file:
                tmp_pinglist_in_memory = yaml.safe_load(file)
                logger.debug(f"{PINGLIST_PATH} yaml was loaded successfully.")

            async with pinglist_lock:
                pinglist_in_memory.clear()
                pinglist_in_memory = copy.deepcopy(tmp_pinglist_in_memory)

        else:
            logger.error(f"Pinglist file not found at {PINGLIST_PATH}")
    except Exception as e:
        logger.error(f"Error loading pinglist: {e}")


async def read_pinglist_periodically():
    load_config_ini()
    try:
        while True:
            await read_pinglist()
            await asyncio.sleep(interval_read_pinglist_sec)
    except asyncio.CancelledError:
        logger.info("read_pinglist_periodically task was cancelled.")
    except Exception as e:
        logger.error(f"Exception in read_pinglist_periodically: {e}")


async def get_pinglist(request):
    client_ip = request.remote
    async with pinglist_lock:
        response_data = pinglist_in_memory
    logger.debug(f"(SEND) pinglist.yaml to client: {client_ip}")
    return web.json_response(response_data)


async def get_address_store(request):
    current_time = int(time.time())
    client_ip = request.remote
    async with address_store_lock:
        global address_store_checkpoint

        # condition to filter old entries (every 1 minute)
        if address_store_checkpoint + 60 < current_time:
            # get keys which was not updated in last 5 minutes
            keys_old_entries = [
                key
                for key, value in address_store.items()
                if value[5] + 300 < current_time
            ]
            for key in keys_old_entries:
                logger.info(f"(EXPIRED) Remove old address information: {key}")
                address_store.pop(key)
            address_store_checkpoint = current_time
        response_data = address_store
    logger.debug(f"(SEND) address_store to client: {client_ip}")
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
        utime = int(time.time())

        if all([ip_address, gid, lid, qpn, dtime]):
            async with address_store_lock:
                address_store[ip_address] = [
                    ip_address,
                    gid,
                    int(lid),
                    int(qpn),
                    str(dtime),
                    int(utime),
                ]
                logger.debug(
                    f"(RECV) POST from {client_ip}. Updated address store (size: {len(address_store)})."
                )

                if len(address_store) > 10000:
                    logger.critical(
                        f"Too many entries in address_store: {len(address_store)}"
                    )
                    logger.error("Cleaning up address_store. Check your configuration.")
                    address_store.clear()
            return web.Response(text="Address updated", status=200)
        else:
            logger.warning(f"(RECV) Incorrect POST format from {client_ip}")
            return web.Response(text="Invalid data", status=400)
    except Exception as e:
        logger.error(f"Error processing POST from {client_ip}: {e}")
        return web.Response(text="Internal webserver error", status=500)


async def index(request):
    # 디렉토리 내의 파일 목록을 가져옵니다.
    files = os.listdir(HTML_DIR)
    # HTML 파일만 필터링합니다.
    html_files = [f for f in files if f.endswith(".html")]
    # 인덱스 페이지를 생성합니다.
    file_links = [f'<li><a href="/{fname}">{fname}</a></li>' for fname in html_files]
    content = f"""
    <html>
        <head><title>Available HTML Files</title></head>
        <body>
            <h1>Available pingmesh list</h1>
            <ul>
                {''.join(file_links)}
            </ul>
        </body>
    </html>
    """
    return web.Response(text=content, content_type="text/html")


async def pingweave_webserver():
    load_config_ini()

    try:
        while True:
            if not check_ip_active(control_host):
                logger.error(
                    f"No active interface with Control IP {control_host}. Sleeping for 1 minute..."
                )
                await asyncio.sleep(60)
                load_config_ini()
                continue

            try:
                app = web.Application()
                app.router.add_get("/", index)  # indexing for html files
                app.router.add_static("/", HTML_DIR)  # static route for html
                app.router.add_get("/pinglist", get_pinglist)
                app.router.add_get("/address_store", get_address_store)
                app.router.add_post("/address", post_address)

                runner = web.AppRunner(app)
                await runner.setup()
                site = web.TCPSite(runner, host="0.0.0.0", port=control_port)
                await site.start()

                logger.info(
                    f"Pingweave webserver running on {control_host}:{control_port}"
                )

                asyncio.create_task(read_pinglist_periodically())

                await asyncio.Event().wait()

            except asyncio.CancelledError:
                logger.info("pingweave_webserver task was cancelled.")
                break
            except Exception as e:
                logger.error(f"Cannot start the pingweave webserver: {e}")
            finally:
                await runner.cleanup()
                await asyncio.sleep(10)
    except KeyboardInterrupt:
        logger.info("pingweave_webserver received KeyboardInterrupt. Exiting.")
    except Exception as e:
        logger.error(f"Exception in pingweave_webserver: {e}")


def run_pingweave_webserver():
    setproctitle("pingweave_webserver.py")
    try:
        asyncio.run(pingweave_webserver())
    except KeyboardInterrupt:
        logger.info("pingweave_webserver process received KeyboardInterrupt. Exiting.")
