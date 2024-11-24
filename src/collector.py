import asyncio
import os
import socket
import configparser
from aiohttp import web
import psutil
from logger import initialize_pingweave_logger

logger = initialize_pingweave_logger(socket.gethostname(), "collector")

# Configuration paths
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
CONFIG_PATH = os.path.join(SCRIPT_DIR, "../config/pingweave.ini")

# Global variables
control_host = None
collect_port = None

# ConfigParser object
config = configparser.ConfigParser()


def load_config_ini():
    global control_host, collect_port

    try:
        config.read(CONFIG_PATH)
        control_host = config["controller"]["host"]
        collect_port = int(config["controller"]["port_collect"])
        logger.debug("Configuration loaded successfully from config file.")
    except Exception as e:
        logger.error(f"Error reading configuration: {e}")
        control_host = "0.0.0.0"
        collect_port = 8080


def check_ip_active(target_ip):
    """
    Checks if the given IP address is active and associated with an interface that is UP.
    """
    try:
        net_if_addrs = psutil.net_if_addrs()
        net_if_stats = psutil.net_if_stats()

        for iface, addrs in net_if_addrs.items():
            for addr in addrs:
                if addr.family == socket.AF_INET and addr.address == target_ip:
                    if iface in net_if_stats and net_if_stats[iface].isup:
                        return True
                    else:
                        logger.error(f"Interface {iface} with IP {target_ip} is down.")
                        return False
        logger.error(f"No active interface found with IP address {target_ip}.")
        return False
    except Exception as e:
        logger.error(f"Error checking IP activity: {e}")
        return False


async def handle_result_rdma_post(request):
    client_ip = request.remote

    try:
        raw_data = await request.text()
        logger.debug(f"Raw POST RESULT data from {client_ip}: {raw_data}")

        results = raw_data.strip().split("\n")
        for result in results:
            data = result.strip().split(",")
            print(f"{data}")

        return web.Response(text="Data processed successfully", status=200)
    except Exception as e:
        logger.error(f"Error processing POST result_rdma from {client_ip}: {e}")
        return web.Response(text="Internal server error", status=500)


async def handle_alarm_post(request):
    client_ip = request.remote
    try:
        raw_data = await request.text()
        logger.info(f"ALARM from {client_ip}: {raw_data}")
        return web.Response(text="Data processed successfully", status=200)
    except Exception as e:
        logger.error(f"Error processing POST alarm from {client_ip}: {e}")
        return web.Response(text="Internal server error", status=500)


async def pingweave_collector():
    load_config_ini()

    try:
        while True:
            if not check_ip_active(control_host):
                logger.info(
                    f"No active interface with Control IP {control_host}. Sleep 1 minute..."
                )
                await asyncio.sleep(60)
                load_config_ini()
                continue

            runner = None
            try:
                app = web.Application()
                app.router.add_post("/result_rdma", handle_result_rdma_post)
                app.router.add_post("/alarm", handle_alarm_post)
                runner = web.AppRunner(app)
                await runner.setup()
                site = web.TCPSite(runner, host="0.0.0.0", port=collect_port)
                await site.start()

                logger.info(
                    f"Pingweave collector running on {control_host}:{collect_port}"
                )
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


def run_pingweave_collector():
    try:
        asyncio.run(pingweave_collector())
    except KeyboardInterrupt:
        logger.info("pingweave_collector process received KeyboardInterrupt. Exiting.")
