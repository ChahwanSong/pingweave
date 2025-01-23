import asyncio
import os
import socket
import configparser
from aiohttp import web  # aiohttp for webserver
import redis  # in-memory key-value storage
import yaml  # python3 -m pip install pyyaml
from datetime import datetime, timedelta
import psutil
from logger import initialize_pingweave_logger
from macro import *
from setproctitle import setproctitle

logger = initialize_pingweave_logger(socket.gethostname(), "collector", 5, False)

# Global variables
control_host = None
collect_port = None

# Variables to save pinglist
pinglist_in_memory = {}

# ConfigParser object
config = configparser.ConfigParser()

try:
    # Redis
    socket_path = "/var/run/redis/redis-server.sock"
    redis_server = redis.StrictRedis(
        unix_socket_path=socket_path, decode_responses=True
    )
    logger.info(f"Redis server running - {redis_server.ping()}")  # 출력: True
    assert redis_server.ping()
    redis_server.flushdb()
    logger.info(f"Redis server cleanup - flushdb()")

except redis.exceptions.ConnectionError as e:
    logger.error(f"Cannot connect to Redis server: {e}")
    if not os.path.exists(socket_path):
        logger.error(f"Socket file does not exist: {socket_path}")
    redis_server = None
except FileNotFoundError as e:
    logger.error(f"Redis socket file does not exist: {e}")
    redis_server = None
except Exception as e:
    logger.error(f"Unexpected error of Redis server: {e}")
    redis_server = None


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


async def process_result_post(request: web.Request, prefix: str) -> web.Response:
    """
    A helper function that encapsulates the common logic of parsing lines from the request,
    building the Redis key/value, and logging errors.
    """
    client_ip = request.remote
    try:
        raw_data = await request.text()
        logger.debug(f"Raw POST RESULT data from {client_ip}: {raw_data}")

        results = raw_data.strip().split("\n")

        if redis_server is not None:
            for line in results:
                data = line.strip().split(",")
                if len(data) < 3:
                    logger.warning(f"Skipping malformed line: {line}")
                    continue
                # e.g., key = "roce,192.168.0.1,192.168.0.2"
                #       value = "ts_start,ts_end,#success,#fail,..."
                key = f"{prefix}," + ",".join(data[:2])
                value = ",".join(data[2:])

                # Because HTTP POST can be delayed at client-side, out-of-order arrival is
                # possible. To avoid showing a old data for redis, we drop the old POST
                # by comparing the 'ts_end'.
                prev_ts_end_raw = redis_server.get(key)
                if prev_ts_end_raw:
                    prev_ts_end = datetime.strptime(
                        prev_ts_end_raw.split(",")[1][:26], "%Y-%m-%d %H:%M:%S.%f"
                    )
                else:
                    prev_ts_end = datetime.min

                # compare with new post time
                new_ts_end = datetime.strptime(data[3][:26], "%Y-%m-%d %H:%M:%S.%f")
                if prev_ts_end < new_ts_end:
                    redis_server.set(key, value)
                else:
                    logger.info(
                        f"{key} - out-of-order post arrival (prev: {prev_ts_end}, new: {new_ts_end})"
                    )

                # TODO: Anyway, we keep all the data in database.

        return web.Response(text="Data processed successfully", status=200)
    except Exception as e:
        logger.error(f"Error processing POST result_{prefix} from {client_ip}: {e}")
        return web.Response(text="Internal server error", status=500)


# Now each protocol-specific endpoint becomes a simple wrapper:
async def handle_result_roce_post(request):
    return await process_result_post(request, "roce")


async def handle_result_ib_post(request):
    return await process_result_post(request, "ib")


async def handle_result_udp_post(request):
    return await process_result_post(request, "udp")


async def handle_result_tcp_post(request):
    return await process_result_post(request, "tcp")


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
                app.router.add_post("/result_roce", handle_result_roce_post)
                app.router.add_post("/result_ib", handle_result_ib_post)
                app.router.add_post("/result_udp", handle_result_udp_post)
                app.router.add_post("/result_tcp", handle_result_tcp_post)
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
    setproctitle("pingweave_collector.py")
    try:
        asyncio.run(pingweave_collector())
    except KeyboardInterrupt:
        logger.info("pingweave_collector process received KeyboardInterrupt. Exiting.")
