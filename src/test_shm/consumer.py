import time
import logging
import ctypes
from multiprocessing import shared_memory, resource_tracker

# Match C++ constants
MESSAGE_SIZE = 1024
BUFFER_SIZE = 1024


# Define the structure to match the C++ SharedData (no message_ready here)
class SharedData(ctypes.Structure):
    _fields_ = [
        ("messages", ctypes.c_char * MESSAGE_SIZE * BUFFER_SIZE),
        ("head", ctypes.c_int),
        ("tail", ctypes.c_int),
    ]


class ConsumerQueue:
    def __init__(self, prefix: str, shm_name: str):
        """
        Initialize and map the existing shared memory created by the producer.
        """
        self.prefix = prefix
        self.shm_name = shm_name
        self.shm = None
        self.shared_data = None

        # Attempt to load the shared memory
        self.load_memory()
        logging.info(f"[{self.shm_name}] ConsumerQueue initialized.")

    def __enter__(self):
        # Just return self so user can do 'with ConsumerQueue(...) as c:'
        return self

    def __exit__(self, exc_type, exc_val, exc_tb):
        # Called automatically when the 'with' block ends
        self.cleanup()

    def __del__(self):
        """
        Destructor: ensure we clean up.
        """
        try:
            self.cleanup()
        except AttributeError:
            pass

    def load_memory(self):
        """
        Maps the shared memory. This must match the producer's name (prefix + shm_name).
        """
        full_name = self.prefix + self.shm_name
        try:
            self.shm = shared_memory.SharedMemory(name=full_name)
            # Attach our ctypes structure to the shared memory buffer
            self.shared_data = SharedData.from_buffer(self.shm.buf)
            # Unregister from Python's resource tracker to avoid auto cleanup
            resource_tracker.unregister(self.shm._name, "shared_memory")
            logging.info(f"Successfully mapped shared memory: {full_name}")
        except FileNotFoundError:
            logging.error(
                f"Shared memory '{full_name}' not found. Make sure the producer is running."
            )
            raise

    def read_messages(self):
        """
        Continuously reads available messages until the ring buffer is empty:
          - While head != tail, read the message at head.
          - Increment head (wrap around with modulo).
        Returns a list of messages read during this call.
        """
        messages = []
        # While there is at least one unread message
        while self.shared_data.head != self.shared_data.tail:
            head_index = self.shared_data.head
            c_char_array = self.shared_data.messages[head_index]
            raw_bytes = bytes(c_char_array)  # Convert c_char_Array_64 -> bytes
            msg_str = raw_bytes.decode("utf-8", "ignore").rstrip("\x00")

            messages.append(msg_str)

            # Move head forward
            new_head = (head_index + 1) % BUFFER_SIZE
            self.shared_data.head = new_head

        return messages

    def cleanup(self):
        """
        Close the shared memory mapping (does not unlink it,
        since the producer might still be using it).
        """
        if self.shm is not None:
            try:
                self.shm.close()
            except BufferError as e:
                logging.warning(f"Shared memory close issue: {e}")
            self.shm = None
            self.shared_data = None
