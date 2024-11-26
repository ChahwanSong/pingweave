import logging
import os
from logging.handlers import RotatingFileHandler

# Create the logs directory if it doesn't exist
log_dir = "../logs"
logging_level = logging.INFO  # INFO
console_level = logging.ERROR  # ERROR

if not os.path.exists(log_dir):
    os.makedirs(log_dir)


def initialize_pingweave_logger(
    host: str, middlename: str = "server", enable_console=True
):
    logger = logging.getLogger(f"pingweave_{middlename}_{host}")
    logger.setLevel(logging_level)  # Set the log level

    if enable_console:
        console_handler = logging.StreamHandler()
        console_handler.setLevel(console_level)  # >=ERROR to console

    log_file = os.path.join(log_dir, f"pingweave_{middlename}_{host}.log")
    file_handler = RotatingFileHandler(
        log_file,
        maxBytes=100 * 1024 * 1024,
        backupCount=0,
    )
    file_handler.setLevel(logging_level)  # >=INFO to file

    formatter = logging.Formatter(
        "[%(asctime)s][%(levelname)s][%(filename)s:%(funcName)s] %(message)s",
        datefmt="%Y-%m-%d %H:%M:%S",
    )

    file_handler.setFormatter(formatter)
    if enable_console:
        console_handler.setFormatter(formatter)

    logger.addHandler(console_handler)
    logger.addHandler(file_handler)

    logger.info(
        f"Logger initialization is successful - pingweave_{middlename}_{host}.log"
    )
    return logger
