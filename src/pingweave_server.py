import os
import sys
import subprocess
import configparser  # default library
import asyncio  # default library
import time
import socket

try:
    import yaml
except ImportError:
    print("PyYAML library not found. Installing...")
    subprocess.check_call([sys.executable, "-m", "pip", "install", "pyyaml"])
    import yaml

from aiohttp import web
from logger import initialize_pinglist_logger

# absolute paths of this script and pinglist.yaml
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

# global variables
control_host = None
control_port = None
interval_sync_pinglist_sec = None
interval_read_pinglist_sec = None

logger = initialize_pinglist_logger(socket.gethostname(), "server")

python_version = sys.version_info
if python_version < (3, 7):
    logger.error(f"Python 3.7 or higher is required. Current version: {sys.version}")
    sys.exit(1)  # 프로그램 종료


def check_ip_active(target_ip):
    try:
        # Linux/Unix - based 'ip addr' command
        result = subprocess.run(
            ["ip", "addr"], capture_output=True, text=True
        )

        if result.returncode != 0:
            logger.error(
                f"Error running 'ip addr' command. Return code: {result.returncode}"
            )
            logger.error(f"Standard error output: {result.stderr.strip()}")
            return False

        # Check if the target_ip is in the 'ip addr' output and if it's not down
        if target_ip in result.stdout:
            # Check if the interface is up or down
            if f"{target_ip}" in result.stdout and "state UP" in result.stdout:
                return True
            else:
                logger.error(f"Interface with IP {target_ip} is down.")
                return False
        else:
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


async def read_pinglist():
    global pinglist_in_memory
    pinglist_in_memory.clear()

    try:
        # use a lock to hold client request during file loading
        async with pinglist_lock:
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
    while True:
        await read_pinglist()
        await asyncio.sleep(interval_read_pinglist_sec)


async def get_pinglist(request):
    client_ip = request.remote
    async with pinglist_lock:
        response_data = pinglist_in_memory
    logger.debug(f"(SEND) pinglist data to client: {client_ip}")
    return web.json_response(response_data)


async def get_address_store(request):
    client_ip = request.remote
    async with address_store_lock:
        response_data = address_store
    logger.debug(f"(SEND) address_store to client: {client_ip}")
    return web.json_response(response_data)


async def post_address(request):
    client_ip = request.remote
    try:
        data = await request.json()
        ip_address = data.get('ip_address')
        gid = data.get('gid')
        lid = data.get('lid')
        qpn = data.get('qpn')
        dtime = data.get('dtime')

        if all([ip_address, gid, lid, qpn, dtime]):
            async with address_store_lock:
                address_store[ip_address] = [ip_address, gid, int(lid), int(qpn)]
                logger.info(f"(RECV) POST from {client_ip}. Update the address store.")

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
        logger.error(f"Error handling POST data from {client_ip}: {e}")
        return web.Response(text="Internal server error", status=500)


async def main():
    # initially load a config file
    load_config_ini()

    # parallel task of loading pinglist file from config file
    asyncio.create_task(read_pinglist_periodically())

    while True:
        if not check_ip_active(control_host):
            logger.error(
                f"No active iface with Control IP {control_host}. Sleep 10 minutes..."
            )
            await asyncio.sleep(600)  # sleep 600 seconds

            # reload a config file and try
            load_config_ini()
            continue

        try:
            app = web.Application()
            app.router.add_get('/pinglist', get_pinglist)
            app.router.add_get('/address_store', get_address_store)
            app.router.add_post('/address', post_address)

            runner = web.AppRunner(app)
            await runner.setup()
            site = web.TCPSite(runner, control_host, control_port)
            await site.start()

            logger.info(f"Pingweave server running on {control_host}:{control_port}")

            # Keep the server running indefinitely
            await asyncio.Event().wait()

        except Exception as e:
            logger.error(f"Cannot start the pingweave server: {e}")
            await asyncio.sleep(10)
        finally:
            await runner.cleanup()


if __name__ == "__main__":
    asyncio.run(main())
