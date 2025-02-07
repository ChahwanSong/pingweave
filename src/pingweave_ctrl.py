#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import sys
import signal
import time
import socket
import multiprocessing

from logger import initialize_pingweave_logger
from macro import *
from common import *

logger = initialize_pingweave_logger(socket.gethostname(), "ctrl", 30, False)

python_version = sys.version_info
if python_version < (3, 7):
    logger.critical(f"Python 3.7 or higher is required. Current version: {sys.version}")
    sys.exit(1)

try:
    from ctrl_collector_http import (
        run_pingweave_collector_http,
    )  # Import the collector function
except ImportError as e:
    logger.error(
        f"Could not import run_pingweave_collector_http from collector.py: {e}"
    )
    sys.exit(1)

try:
    from ctrl_collector_zmq import (
        run_pingweave_collector_zmq,
    )  # Import the collector function
except ImportError as e:
    logger.error(f"Could not import run_pingweave_collector_zmq from collector.py: {e}")
    sys.exit(1)

try:
    from ctrl_plotter import run_pingweave_plotter  # Import the plotter function
except ImportError as e:
    logger.error(f"Could not import run_pingweave_plotter from plotter.py: {e}")
    sys.exit(1)

try:
    from ctrl_webserver import run_pingweave_webserver  # Import the webserver function
except ImportError as e:
    logger.error(f"Could not import run_pingweave_webserver from webserver.py: {e}")
    sys.exit(1)


if __name__ == "__main__":
    # List to hold all processes
    processes = []

    try:
        # Define processes
        process_server = multiprocessing.Process(
            target=run_pingweave_webserver, name="pingweave_webserver", daemon=True
        )
        process_plotter = multiprocessing.Process(
            target=run_pingweave_plotter, name="pingweave_plotter", daemon=True
        )
        process_collector_http = multiprocessing.Process(
            target=run_pingweave_collector_http,
            name="pingweave_collector_http",
            daemon=True,
        )
        process_collector_zmq = multiprocessing.Process(
            target=run_pingweave_collector_zmq,
            name="pingweave_collector_zmq",
            daemon=True,
        )

        processes = [
            process_server,
            process_collector_http,
            process_collector_zmq,
            process_plotter,
        ]

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