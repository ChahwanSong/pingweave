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

logger = initialize_pingweave_logger(socket.gethostname(), "server", 30, False)

# ConfigParser object
config = configparser.ConfigParser()

python_version = sys.version_info
if python_version < (3, 7):
    logger.critical(f"Python 3.7 or higher is required. Current version: {sys.version}")
    sys.exit(1)

try:
    from collector import run_pingweave_collector  # Import the collector function
except ImportError as e:
    logger.error(f"Could not import run_pingweave_collector from collector.py: {e}")
    sys.exit(1)

try:
    from plotter import run_pingweave_plotter  # Import the plotter function
except ImportError as e:
    logger.error(f"Could not import run_pingweave_plotter from plotter.py: {e}")
    sys.exit(1)

try:
    from webserver import run_pingweave_webserver  # Import the webserver function
except ImportError as e:
    logger.error(f"Could not import run_pingweave_webserver from webserver.py: {e}")
    sys.exit(1)


def terminate_all(processes):
    """
    Terminates all running processes gracefully.
    """
    for process in processes:
        if process.is_alive():
            process.terminate()
            logger.warning(f"Terminated process: {process.name}")


if __name__ == "__main__":
    # List to hold all processes
    processes = []

    try:
        # Define processes
        process_server = multiprocessing.Process(
            target=run_pingweave_webserver, name="pingweave_webserver", daemon=True
        )
        process_collector = multiprocessing.Process(
            target=run_pingweave_collector, name="pingweave_collector", daemon=True
        )
        process_plotter = multiprocessing.Process(
            target=run_pingweave_plotter, name="pingweave_plotter", daemon=True
        )

        processes = [process_server, process_collector, process_plotter]

        # Start processes
        for process in processes:
            process.start()

        def signal_handler(sig, frame):
            """
            Signal handler to gracefully terminate all processes.
            """
            logger.info(f"Received signal {sig}. Terminating all processes...")
            terminate_all(processes)
            sys.exit(0)

        # Register signal handlers
        signal.signal(signal.SIGINT, signal_handler)  # Handle Ctrl+C
        signal.signal(signal.SIGTERM, signal_handler)  # Handle termination signals

        while True:
            for process in processes:
                if not process.is_alive():
                    logger.warning(f"{process.name} has stopped unexpectedly.")
                    terminate_all(processes)
                    sys.exit(1)
            time.sleep(1)

    except Exception as e:
        logger.error(f"Main loop exception: {e}. Exiting cleanly...")
    finally:
        terminate_all(processes)