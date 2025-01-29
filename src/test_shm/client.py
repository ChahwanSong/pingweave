from consumer import *
import logging


def main():
    logging.basicConfig(
        level=logging.INFO, format="%(asctime)s [%(levelname)s] %(message)s"
    )

    # Adjust these to match the producer's prefix and name
    prefix = "pingweave"
    shm_name = "10.200.200.3"

    try:
        consumer = ConsumerQueue(prefix, shm_name)
    except FileNotFoundError:
        # If the shared memory wasn't found, exit
        return

    try:
        while True:
            # Read all available messages
            msgs = consumer.read_messages()
            if msgs:
                # Print (or otherwise handle) the messages
                for m in msgs:
                    print(f"Consumer got message: {m}")
            # else:
            #     # If no messages, sleep briefly
            #     time.sleep(0.001)

    except KeyboardInterrupt:
        logging.info("Consumer interrupted. Exiting...")


if __name__ == "__main__":
    main()
