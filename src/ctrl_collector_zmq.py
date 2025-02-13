#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import os
import sys
import socket
import redis  # in-memory key-value storage
import zmq
import threading

from logger import initialize_pingweave_logger
from macro import *
from setproctitle import setproctitle
from common import *

logger = initialize_pingweave_logger(
    socket.gethostname(), "ctrl_collector_zmq", 5, False
)

try:
    # Attempt to connect to Redis via Unix socket
    redis_server = redis.StrictRedis(
        unix_socket_path=REDIS_SOCKET_PATH, decode_responses=True
    )
    logger.info(f"Redis server running - {redis_server.ping()}")
    assert redis_server.ping()
    # # Flush all data in the current Redis database
    # redis_server.flushdb()
    # logger.info("Redis server cleanup - flushdb()")

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


def worker_routine(worker_url, worker_id):
    """
    Function executed by each worker thread.
    It communicates with the server (backend) via a DEALER socket.
    """
    context = zmq.Context.instance()
    socket = context.socket(zmq.DEALER)
    socket.connect(worker_url)

    while True:
        # Receive multipart messages from ROUTER -> (proxy) -> DEALER
        # Typically, we expect 3 frames: [client_id, empty, actual_data]
        try:
            frames = socket.recv_multipart()
        except Exception as e:
            logger.exception(f"[Worker {worker_id}] Error receiving frames: {e}")
            # If there's a receive error, skip this iteration and try again
            continue

        # Check the number of frames for robust handling
        if len(frames) != 3:
            logger.warning(f"[Worker {worker_id}] Unexpected frames: {frames}")
            continue

        client_id, empty, msg = frames

        # Convert the clientID, empty, and message to a readable string (or hex)
        try:
            client_id_str = client_id.decode("utf-8", errors="replace")
            msg_str = msg.decode("utf-8", errors="replace")

            # Example: the client_id is expected to have format "protocol_ipv4"
            # This can raise a ValueError if split("_") fails.
            protocol, ipv4 = client_id_str.strip().split("_")

        except ValueError as ve:
            logger.error(
                f"[Worker {worker_id}] Error parsing client_id_str '{client_id_str}': {ve}"
            )
            # Skip this message and continue receiving next
            continue
        except Exception as e:
            logger.error(
                f"[Worker {worker_id}] Unexpected error parsing frames: {e}"
            )
            continue

        # Process the message (e.g. split into lines)
        results = msg_str.strip().split("\n")

        # Store or log data in Redis if available
        try:
            process_collected_message(redis_server, results, protocol, logger)
        except Exception as e:
            pass # no error handling

        # Send response: [client_id, rest_api, reply_data]
        try:
            reply_data = "Success.".encode("utf-8")
            socket.send_multipart([client_id, b"", reply_data])
        except Exception as e:
            logger.error(f"[Worker {worker_id}] Error sending reply: {e}")
            continue


def pingweave_collector_zmq():
    """
    Main function that sets up the Router (frontend) and Dealer (backend),
    spawns worker threads, and starts the ZeroMQ proxy.
    """
    context = zmq.Context.instance()

    # 1) ROUTER socket (frontend) for client connections
    frontend = context.socket(zmq.ROUTER)
    frontend.bind(f"tcp://{config['control_host']}:{config['collect_port_zmq']}")

    # 2) DEALER socket (backend, inproc) to communicate with workers
    backend = context.socket(zmq.DEALER)
    backend.bind("inproc://workers")

    # 3) Create 4 worker threads
    num_workers = 4
    for i in range(num_workers):
        t = threading.Thread(
            target=worker_routine, args=("inproc://workers", i), daemon=True
        )
        t.start()

    # 4) Forward messages between frontend(ROUTER) and backend(DEALER)
    zmq.proxy(frontend, backend)

    # Normally, we never reach here because proxy() blocks indefinitely
    frontend.close()
    backend.close()
    context.term()


def run_pingweave_collector_zmq():
    """
    Entry point to run the collector.
    Sets the process title and handles KeyboardInterrupt for graceful exit.
    """
    setproctitle("pingweave_ctrl_collector_zmq.py")
    try:
        pingweave_collector_zmq()
    except KeyboardInterrupt:
        logger.info(
            "pingweave_collector_zmq process received KeyboardInterrupt. Exiting."
        )
