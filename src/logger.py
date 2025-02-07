import logging
import os
from logging.handlers import RotatingFileHandler
from macro import *

# Create the logs directory if it doesn't exist
# TODO: read from pingweave.ini

logging_level = logging.INFO
console_level = logging.INFO

if not os.path.exists(LOG_DIR):
    os.makedirs(LOG_DIR)


def initialize_pingweave_logger(
    host: str, middlename: str = "server", max_MB=30, enable_console=True
):
    logger = logging.getLogger(f"pingweave_{middlename}_{host}")

    # Clear any existing handlers so we start fresh
    logger.handlers = []

    # Prevent log messages from propagating to ancestor loggers (like the root logger)
    logger.propagate = False

    # Set the log level for this logger
    logger.setLevel(logging_level)

    # 1) File handler
    log_file = os.path.join(LOG_DIR, f"pingweave_{middlename}_{host}.log")
    file_handler = RotatingFileHandler(
        log_file,
        maxBytes=int(max_MB) * 1024 * 1024,
        backupCount=1,
    )
    file_handler.setLevel(logging_level)  # >=INFO to file
    formatter = logging.Formatter(
        "[%(asctime)s][%(levelname)s][%(filename)s:%(funcName)s] %(message)s",
        datefmt="%Y-%m-%d %H:%M:%S",
    )
    file_handler.setFormatter(formatter)
    logger.addHandler(file_handler)

    if enable_console:
        console_handler = logging.StreamHandler()
        console_handler.setLevel(console_level)  # >=ERROR to console
        console_handler.setFormatter(formatter)
        logger.addHandler(console_handler)

    mypid = os.getpid()
    logger.info(
        f"Logger initialization is successful - pingweave_{middlename}_{host}.log (pid: {mypid})"
    )
    return logger
