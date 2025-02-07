#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import multiprocessing
import sys
import time
import signal
import socket
import configparser

from common import *
from macro import *
from logger import initialize_pingweave_logger

logger = initialize_pingweave_logger(socket.gethostname(), "agent", 10, False)

python_version = sys.version_info
if python_version < (3, 6):
    logger.critical(f"Python 3.6 or higher is required. Current version: {sys.version}")
    sys.exit(1)

try:
    from agent_fetcher import run_agent_fetcher
except ImportError as e:
    logger.error(f"Could not import run_agent_fetcher from agent_fetcher.py: {e}")
    sys.exit(1)

try:
    from agent_sender import run_agent_sender
except ImportError as e:
    logger.error(f"Could not import run_agent_sender from agent_sender.py: {e}")
    sys.exit(1)


if __name__ == "__main__":
    # process list
    processes = []

    try:
        process_fetcher = multiprocessing.Process(
            target=run_agent_fetcher,
            name="agent_fetcher",
            daemon=True,
        )
        processes.append(process_fetcher)

        process_sender = multiprocessing.Process(
            target=run_agent_sender,
            name="agent_sender",
            daemon=True,
        )
        processes.append(process_sender)

        # Start processes
        for process in processes:
            process.start()

        def signal_handler(sig, frame):
            """
            Signal handler to gracefully terminate all processes.
            """
            logger.info(f"Received signal {sig}. Terminating all processes...")
            terminate_multiprocesses(processes, logger)
            sys.exit(0)

        # Register signal handlers
        signal.signal(signal.SIGINT, signal_handler)  # Handle Ctrl+C
        signal.signal(signal.SIGTERM, signal_handler)  # Handle termination signals

        while True:
            for process in processes:
                if not process.is_alive():
                    logger.warning(f"{process.name} has stopped unexpectedly.")
                    terminate_multiprocesses(processes, logger)
                    sys.exit(1)
            time.sleep(1)

    except Exception as e:
        logger.error(f"Main loop exception: {e}. Exiting cleanly...")
        terminate_multiprocesses(processes, logger)