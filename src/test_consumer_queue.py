import os, sys

# current_dir = os.path.dirname(os.path.abspath(__file__))
# sys.path.append(os.path.join(current_dir, "../src"))

from consumer_queue import *


# main_consumer.py
def main():
    consumer = ConsumerQueue("my_shared_memory_queue")

    # check if no data for a long time
    wait_time = 0
    check_interval = 0.001  # seconds

    if consumer.shm is None:
        queue_logger.error("Shared memory not initialized, cannot receive messages.")
        return

    try:
        while True:
            while not consumer.shared_data.message_ready:
                time.sleep(check_interval)
                wait_time += check_interval

                if wait_time > 10:
                    consumer.reload_memory()
                    wait_time = 0

            msg_list = consumer.process_batch()
            print(f"Python: Last received message in batch: {msg_list[-1]}")
    except KeyboardInterrupt:
        queue_logger.error(
            "KeyboardInterrupt detected. Cleaning up shared memory and exiting..."
        )
    except Exception as e:
        queue_logger.error(f"{e}")
        raise e
    finally:
        consumer.clean_up()


if __name__ == "__main__":
    main()
