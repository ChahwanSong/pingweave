from consumer_queue import *
from logger import *


# main_consumer.py
def main():
    consumer = ConsumerQueue("rdma", "10.200.200.3")

    # check if no data for a long time
    wait_time = 0
    check_interval = 0.001  # seconds

    if consumer.shm is None:
        logging.error("Shared memory not initialized, cannot receive messages.")
        return

    try:
        while True:
            while not consumer.shared_data.message_ready:
                time.sleep(check_interval)
                wait_time += check_interval

                if wait_time > 5:
                    consumer.reload_memory()
                    wait_time = 0

            msg_list = consumer.process_batch()
            print(
                f"Python: Last received message in batch: {msg_list[-1]}, num: {len(msg_list)}"
            )
    except KeyboardInterrupt:
        logging.error(
            "KeyboardInterrupt detected. Cleaning up shared memory and exiting..."
        )
    except Exception as e:
        logging.error(f"{e}")
        raise e
    finally:
        consumer.clean_up()


if __name__ == "__main__":
    main()
