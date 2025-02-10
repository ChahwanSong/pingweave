#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import asyncio
from aiohttp import web  # aiohttp for webserver
import redis  # in-memory key-value storage

from logger import initialize_pingweave_logger
from macro import *
from setproctitle import setproctitle
from common import *

logger = initialize_pingweave_logger( socket.gethostname(), "ctrl_collector_http", 5, False)

try:
    # Redis
    redis_server = redis.StrictRedis(
        unix_socket_path=REDIS_SOCKET_PATH, decode_responses=True
    )
    logger.info(f"Redis server running - {redis_server.ping()}")
    assert redis_server.ping()
    redis_server.flushdb()
    logger.info(f"Redis server cleanup - flushdb()")

except redis.exceptions.ConnectionError as e:
    logger.error(f"Cannot connect to Redis server: {e}")
    if not os.path.exists(REDIS_SOCKET_PATH):
        logger.error(f"Socket file does not exist: {REDIS_SOCKET_PATH}")
    redis_server = None
except FileNotFoundError as e:
    logger.error(f"Redis socket file does not exist: {e}")
    redis_server = None
except Exception as e:
    logger.error(f"Unexpected error of Redis server: {e}")
    redis_server = None

async def process_result_post(request: web.Request, protocol: str) -> web.Response:
    """
    A helper function that encapsulates the common logic of parsing lines from the request,
    building the Redis key/value, and logging errors.
    """
    client_ip = request.remote
    try:
        raw_data = await request.text()
        logger.debug(f"Raw POST RESULT data from {client_ip}: {raw_data}")

        results = raw_data.strip().split("\n")
        try:
            process_collected_message(redis_server, results, protocol, logger) 
        except Exception as e:
            pass # no error handling

        return web.Response(text="Data processed successfully", status=200)
    
    except Exception as e:
        logger.error(f"Error processing POST result_{protocol} from {client_ip}: {e}")
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


async def pingweave_collector_http():
    runner = None

    try:
        while True:
            try:
                app = web.Application()
                app.router.add_post("/result_roce", handle_result_roce_post)
                app.router.add_post("/result_ib", handle_result_ib_post)
                app.router.add_post("/result_udp", handle_result_udp_post)
                app.router.add_post("/result_tcp", handle_result_tcp_post)
                app.router.add_post("/alarm", handle_alarm_post)
                runner = web.AppRunner(app)
                await runner.setup()
                site = web.TCPSite(runner, host="0.0.0.0", port=config["collect_port_http"])
                await site.start()

                logger.info(
                    f"Pingweave collector running on {config['control_host']}:{config['collect_port_http']}"
                )
                await asyncio.Event().wait()

            except asyncio.CancelledError:
                logger.info("pingweave_collector_http task was cancelled.")
                break
            except Exception as e:
                logger.error(f"Cannot start the pingweave collector: {e}")
            finally:
                if runner:
                    await runner.cleanup()
                await asyncio.sleep(10)
    except KeyboardInterrupt:
        logger.info("pingweave_collector_http received KeyboardInterrupt. Exiting.")
    except Exception as e:
        logger.error(f"Exception in pingweave_collector_http: {e}")


def run_pingweave_collector_http():
    setproctitle("pingweave_ctrl_collector_http.py")
    try:
        asyncio.run(pingweave_collector_http())
    except KeyboardInterrupt:
        logger.info(
            "pingweave_collector_http process received KeyboardInterrupt. Exiting."
        )
