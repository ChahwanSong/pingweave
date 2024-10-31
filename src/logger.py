import logging
import os
from logging.handlers import RotatingFileHandler

# Create the logs directory if it doesn't exist
log_dir = "../logs"
queue_log_level = logging.INFO

if not os.path.exists(log_dir):
    os.makedirs(log_dir)


# Initialize a logger object and return
def initialize_consumer_logger(prefix, ipv4):
    # Set up the logger
    logger = logging.getLogger(f"{prefix}_consumer_{ipv4}")
    logger.setLevel(queue_log_level)  # Set the log level

    # Create a rotating file handler for the logger
    log_file = os.path.join(log_dir, f"{prefix}_consumer_{ipv4}.log")

    # Set maxBytes to 5MB (5 * 1024 * 1024) and backupCount to 3 (maximum 3 files)
    file_handler = RotatingFileHandler(
        log_file, maxBytes=5 * 1024 * 1024, backupCount=3
    )
    file_handler.setLevel(queue_log_level)

    # Create a logging format
    formatter = logging.Formatter(
        "[%(asctime)s][%(levelname)s][%(filename)s:%(lineno)d] %(message)s",
        datefmt="%Y-%m-%d %H:%M:%S",
    )
    file_handler.setFormatter(formatter)

    # Add the file handler to the logger
    logger.addHandler(file_handler)

    logger.info("Initialization is successful.")
    # # Example usage of the logger
    # logger.info("This is an info message")
    # logger.debug("This is a debug message")
    # logger.error("This is an error message")
    return logger
