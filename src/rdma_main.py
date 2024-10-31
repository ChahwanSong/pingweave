import os
import subprocess
import sys
from consumer_queue import *
from rdma_common import *

try:
    import schedule
except ImportError:
    print("Moudle 'schedule' is not found. Installing...")
    subprocess.check_call([sys.executable, "-m", "pip", "install", "schedule"])
    import schedule

import time

# from logger import ...

# TODO: aiohttp client

check_interval = 0.001  # seconds


def main():
    ip_addr = "10.200.200.3"

    consumer = ConsumerQueue("rdma", ip_addr)
    if consumer.shm is None:
        consumer.clean_up()
        raise RuntimeError("Shared memory not initialized, cannot receive messages.")

    try:
        while True:
            # check if no data for a long time
            wait_time = 0
            while not consumer.shared_data.message_ready:
                time.sleep(check_interval)
                wait_time += check_interval

                if wait_time > 5:
                    consumer.reload_memory()
                    break

            # poll if message-ready
            msg_list = consumer.process_batch()
            if msg_list:
                print(
                    f"Received message in batch - First: {msg_list[0]}, Last: {msg_list[-1]}"
                )
                # send this to controller

            # poll the pinglist and GID/QPN information

    except KeyboardInterrupt as e:
        print("KeyboardInterrupt detected. Clean up shared memory and exit...")
        consumer.clean_up()
    except Exception as e:
        print(e)
        consumer.clean_up()
        raise e


if __name__ == "__main__":
    main()
