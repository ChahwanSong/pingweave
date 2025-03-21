#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import sys
import signal
import time
import socket
import redis  # in-memory key-value storage
import threading

from logger import initialize_pingweave_logger
from macro import *
from setproctitle import setproctitle
from common import *

logger = initialize_pingweave_logger(socket.gethostname(), "ctrl_consumer", 10, False)

last_id = {proto: "0-0" for proto in TARGET_PROTOCOLS}

def consumer(stream):
    """
    Subscribe each stream (channel). Each consumer makes individual redis connection.
    """
    try:
        # Attempt to connect to Redis via Unix socket
        redis_server = redis.StrictRedis(
            unix_socket_path=REDIS_SOCKET_PATH, decode_responses=True
        )
        logger.info(f"Redis server running - {redis_server.ping()}")
        assert redis_server.ping()
        
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

    if not redis_server:
        return

    protocol = stream.split("_")[-1]
    global last_id

    try:
        logger.info(f"[Consumer-{stream}] Start listening: Stream '{stream}'")
        
        while True:
            messages = redis_server.xread({stream: last_id[protocol]}, block=1, count=100)
            
            if messages:
                # messages format: [(stream_name, [(message_id, {message: line}), ...]), ...]
                for stream, msgs in messages:
                    for msg in msgs:
                        message_id, data = msg
                        logger.debug(f"[Consumer-{stream}] Received: {message_id} -> {data}")
                        
                        # update the last message ID
                        last_id[protocol] = message_id

                        # TODO: fill the logic you want to process messages
                        


    except Exception as e:
        logger.error(f"[Consumer-{stream}] Exception: {e}")

def start_consumer_thread(stream):
    t = threading.Thread(target=consumer, args=(stream,), daemon=True)
    t.start()
    return t

def pingweave_consumer():
    streams = []
    threads = {}
    for protocol in TARGET_PROTOCOLS:
        streams.append(f"{REDIS_STREAM_PREFIX}_{protocol}")
    
    # Start a consumer thread for each stream
    for stream in streams:
        threads[stream] = start_consumer_thread(stream)
    
    try:
        # 스레드 상태를 주기적으로 모니터링
        while True:
            for stream in streams:
                t = threads[stream]
                if not t.is_alive():
                    logger.info(f"Consumer thread for stream '{stream}' was terminated. Resume after 1 second.")
                    time.sleep(1)
                    threads[stream] = start_consumer_thread(stream)
                    logger.info(f"Consumer thread for stream '{stream}' is restarted.")
            time.sleep(1) # monitoring interval
    except KeyboardInterrupt:
        logger.info("Graceful shutdown by KeyboardInterrupt.")

def run_pingweave_consumer():

    """
    Entry point to run the consumer.
    Sets the process title and handles KeyboardInterrupt for graceful exit.
    """
    setproctitle("pingweave_ctrl_consumer.py")
    try:
        pingweave_consumer()
    except KeyboardInterrupt:
        logger.info(
            "pingweave_ctrl_consumer process received KeyboardInterrupt. Exiting."
        )



    