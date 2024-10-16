import time
from multiprocessing import shared_memory
from multiprocessing import resource_tracker
import ctypes
from logger import queue_logger

MESSAGE_SIZE = 64  # Message size of 64 bytes
BATCH_SIZE = 1000  # Process messages in batches of 10
QUEUE_SIZE = BATCH_SIZE + 1  # Queue size


# Define the structure for shared data to match the C++ structure
class SharedData(ctypes.Structure):
    _fields_ = [
        ("messages", ctypes.c_char * MESSAGE_SIZE * QUEUE_SIZE),
        ("head", ctypes.c_int),
        ("tail", ctypes.c_int),
        ("message_ready", ctypes.c_bool),
    ]


class ConsumerQueue:
    # states
    shm = None
    shared_data = None

    def __init__(self, shm_name):
        self.shm_name = shm_name
        # free the resource if pre-allocated
        if self.shm is not None:
            try:
                self.shm.close()
            except Exception as e:
                print(f"Error during cleanup: {e}")

        queue_logger.debug("Created the ConsumerQueue.")
        self.load_memory()

    def load_memory(self):
        try:
            self.shm = shared_memory.SharedMemory(name=self.shm_name)
            self.shared_data = SharedData.from_buffer(self.shm.buf)
            # Disable resource_tracker for shared memory
            resource_tracker.unregister(self.shm._name, "shared_memory")
            queue_logger.debug("(re)loaded the shared memory.")
        except FileNotFoundError as e:
            queue_logger.error(
                f"Shared memory '{self.shm_name}' not found. Ensure the producer is running."
            )
            self.shm = None
            raise e

    def reload_memory(self):
        queue_logger.warning(f"Reload the shared memory at /dev/shm/{self.shm_name}")
        self.clean_up()
        self.load_memory()

    def process_batch(self) -> list:
        msg_list = list()
        messages_in_batch = 0

        while messages_in_batch < BATCH_SIZE:
            if self.shared_data.head == self.shared_data.tail:
                break
            msg = (
                bytes(self.shared_data.messages[self.shared_data.head])
                .decode("utf-8")
                .strip("\x00")
            )
            msg_list.append(msg)
            self.shared_data.head = (self.shared_data.head + 1) % QUEUE_SIZE
            messages_in_batch += 1

        self.shared_data.message_ready = False
        return msg_list

    def clean_up(self):
        if self.shm is not None:
            try:
                # close a shared memory but do not unlink -> keep C++ shared memory
                self.shared_data = None
                self.shm.close()  # 메모리 맵핑 해제
                # self.shm.unlink()  # 공유 메모리 삭제 생략
            except BufferError as e:
                queue_logger.error(f"Error during cleanup: {e}")
            except Exception as e:
                queue_logger.error(f"Unexpected error during cleanup: {e}")

    def __del__(self):
        try:
            self.clean_up()
        except AttributeError:
            pass
