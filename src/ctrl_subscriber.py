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

logger = initialize_pingweave_logger(socket.gethostname(), "ctrl_subscriber", 10, False)


def consumer(topic):
    """
    Subscribe each topic (channel). Each consumer makes individual redis connection.
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

    try:
        pubsub = redis_server.pubsub()
        pubsub.subscribe(topic)
        logger.info(f"[Consumer-{topic}] Start listening: Topic '{topic}'")

        # infinite loop
        for message in pubsub.listen():
            if message['type'] != 'message':
                continue
            
            logger.info(f"[Consumer-{topic}] Received: {message['data']}")

            # TODO: fill the logic you want to process messages


    except Exception as e:
        logger.error(f"[Consumer-{topic}] Exception: {e}")

def start_consumer_thread(topic):
    t = threading.Thread(target=consumer, args=(topic,), daemon=True)
    t.start()
    return t

def pingweave_subscriber():
    topics = []
    threads = {}
    for protocol in TARGET_PROTOCOLS:
        topics.append(f"{REDIS_TOPIC_PINGWEAVE_RESULT}_{protocol}")
    
    # Start a consumer thread for each topic
    for topic in topics:
        threads[topic] = start_consumer_thread(topic)
    
    try:
        # 스레드 상태를 주기적으로 모니터링
        while True:
            for topic in topics:
                t = threads[topic]
                if not t.is_alive():
                    logger.info(f"Consumer thread for topic '{topic}' was terminated. Resume after 1 second.")
                    time.sleep(1)
                    threads[topic] = start_consumer_thread(topic)
                    logger.info(f"Consumer thread for topic '{topic}' is restarted.")
            time.sleep(1) # monitoring interval
    except KeyboardInterrupt:
        logger.info("Graceful shutdown by KeyboardInterrupt.")

def run_pingweave_subscriber():

    """
    Entry point to run the subscriber.
    Sets the process title and handles KeyboardInterrupt for graceful exit.
    """
    setproctitle("pingweave_ctrl_subscriber.py")
    try:
        pingweave_subscriber()
    except KeyboardInterrupt:
        logger.info(
            "pingweave_ctrl_subscriber process received KeyboardInterrupt. Exiting."
        )



    