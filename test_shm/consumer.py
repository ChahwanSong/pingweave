import time
from multiprocessing import shared_memory
import ctypes

QUEUE_SIZE = 1000  # Queue size
MESSAGE_SIZE = 64  # Message size of 64 bytes
BATCH_SIZE = 10  # Process messages in batches of 10


# Define the structure for shared data to match the C++ structure
class SharedData(ctypes.Structure):
    _fields_ = [
        ("messages", ctypes.c_char * MESSAGE_SIZE * QUEUE_SIZE),  # Queue of messages
        ("head", ctypes.c_int),  # Head of the queue (consumer reads from here)
        ("tail", ctypes.c_int),  # Tail of the queue (producer writes to here)
        (
            "message_ready",
            ctypes.c_bool,
        ),  # Flag to indicate messages are ready to be processed
    ]


class Consumer:
    def __init__(self, shm_name):
        try:
            # Attach to the shared memory created by the C++ producer
            self.shm = shared_memory.SharedMemory(name=shm_name)
            self.shared_data = SharedData.from_buffer(self.shm.buf)
        except FileNotFoundError as e:
            print(
                f"Error: Shared memory '{shm_name}' not found. Ensure the producer is running."
            )
            self.shm = None  # Set to None to avoid cleanup issues
            raise e

    def receive_messages(self, total_messages):
        if self.shm is None:
            print("Shared memory not initialized, cannot receive messages.")
            return

        received_messages = 0
        start_time = time.time()

        try:
            while received_messages < total_messages:
                # Wait for the producer to signal that messages are ready
                while not self.shared_data.message_ready:
                    time.sleep(0.00005)  # Small sleep to reduce CPU usage

                # Process the available batch of messages
                self.process_batch(received_messages, total_messages)

            # Measure throughput
            elapsed_time = time.time() - start_time
            throughput = total_messages / elapsed_time
            print(f"Python: Throughput: {throughput} messages/second")

        except KeyboardInterrupt:
            print(
                "\nKeyboardInterrupt detected. Cleaning up shared memory and exiting..."
            )

        finally:
            self.clean_up()

    def process_batch(self, received_messages, total_messages):
        messages_in_batch = 0

        while messages_in_batch < BATCH_SIZE and received_messages < total_messages:
            if self.shared_data.head == self.shared_data.tail:
                break  # No more messages to process

            # Read the message at the head position
            message = (
                bytes(self.shared_data.messages[self.shared_data.head])
                .decode("utf-8")
                .strip("\x00")
            )
            print(f"Python: Received message {received_messages + 1}: {message}")

            # Update the head to the next position
            self.shared_data.head = (self.shared_data.head + 1) % QUEUE_SIZE

            received_messages += 1
            messages_in_batch += 1

        # Reset the flag after processing the batch
        if messages_in_batch > 0:
            self.shared_data.message_ready = False

    def clean_up(self):
        if self.shm is not None:
            try:
                # Cleanup shared memory
                self.shm.close()
                self.shm.unlink()  # Explicitly unlink the shared memory object
            except BufferError as e:
                print(f"Error during cleanup: {e}")
            except Exception as e:
                print(f"Unexpected error during cleanup: {e}")

    def __del__(self):
        try:
            self.clean_up()
        except AttributeError:
            pass  # Ignore the cleanup if 'shm' attribute is not set


# main_consumer.py
def main():
    total_messages = 100000  # Match the total number of messages in C++
    consumer = Consumer("my_shared_memory_queue")

    # Start receiving messages
    consumer.receive_messages(total_messages)


if __name__ == "__main__":
    main()
