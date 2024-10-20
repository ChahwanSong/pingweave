from consumer_queue import *

# TODO: aiohttp API logger


def main():
    ip_addr = "10.200.200.3"
    check_interval = 0.001  # seconds

    consumer = ConsumerQueue(ip_addr)
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

            msg_list = consumer.process_batch()
            if msg_list:
                print(
                    f"Received message in batch - First: {msg_list[0]}, Last: {msg_list[-1]}"
                )
    except KeyboardInterrupt as e:
        print("KeyboardInterrupt detected. Cleaning up shared memory and exiting...")
        consumer.clean_up()
        raise RuntimeError(
            "KeyboardInterrupt detected. Cleaning up shared memory and exiting..."
        )
    except Exception as e:
        print(e)
        consumer.clean_up()
        raise e


if __name__ == "__main__":
    main()
