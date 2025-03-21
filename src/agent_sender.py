#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import os
import sys
import time
import threading

from ipc_consumer import *
from macro import *
from common import *
from logger import initialize_pingweave_logger

logger = initialize_pingweave_logger(socket.gethostname(), "agent_sender", 10, False)

# Python version check
python_version = sys.version_info
if python_version < (3, 6):
    logger.critical("Python 3.6 or higher is required. Current version: {sys.version}")
    sys.exit(1)


def thread_process_messages(protocol, ipv4, stop_event):
    """
    Consumes messages from the shared memory via ConsumerQueue
    and processes them. The 'daemon=True' option allows this
    thread to run in the background.
    """
    logger.info(f"[{protocol}:{ipv4}] Start a new thread to process messages")

    # context of ZeroMQ
    zmq_context = None

    # address a controller
    control_host = config["control_host"]
    collect_port_zmq = config["collect_port_zmq"]
    collect_port_http = config["collect_port_http"]

    # load ZeroMQ library
    protocol_to_report_result = config["protocol_to_report_result"]
    if protocol_to_report_result not in ["http", "zmq"]:
        logger.warning(
            "Invalid protocol to report results: {}. Use a default protocol - http",
            protocol_to_report_result,
        )

    if protocol_to_report_result == "zmq":
        try:
            import zmq

            zmq_context = zmq.Context()

            def send_message_via_zmq(message: str, protocol: str, ipv4: str):
                latency = None
                try:
                    start_time = time.perf_counter()
                    zmq_socket = zmq_context.socket(zmq.DEALER)  # socket=DEALER

                    # Set a client identity
                    client_id = f"{protocol}_{ipv4}"
                    zmq_socket.setsockopt(zmq.IDENTITY, client_id.encode("ascii"))

                    # Connect to ROUTER
                    zmq_socket.connect(f"tcp://{control_host}:{collect_port_zmq}")

                    # Register a poller : POLLIN(readable event)
                    poller = zmq.Poller()
                    poller.register(zmq_socket, zmq.POLLIN)

                    # UTF-8 Encoding
                    msg_bytes = message.encode("utf-8")

                    # Multipart transmission from DEALER to ROUTER: [Middle frame, Actual message]
                    #  - The ROUTER socket does not automatically add or recognize the client ID
                    #    in the first frame (unlike REQ/REP).
                    #  - However, in a "proxy" environment, the ROUTER retains the client ID as a separate frame.
                    #  - By convention, sending in the format of [empty, "message"]
                    #    results in the worker receiving [client ID, empty, message] on the server side.
                    zmq_socket.send_multipart([b"", msg_bytes])
                    logger.debug(f"[{protocol}:{ipv4}] ZMQ client sends: {message}")

                    # set timeout (3 seconds)
                    timeout_ms = 3000
                    socks = dict(poller.poll(timeout_ms))
                    if zmq_socket in socks and socks[zmq_socket] == zmq.POLLIN:
                        # received a response
                        # format: [server ID, empty frame, response message]
                        reply_frames = zmq_socket.recv_multipart(zmq.NOBLOCK)
                        reply_server = reply_frames[0].decode("utf-8", errors="replace")
                        reply_text = reply_frames[-1].decode("utf-8", errors="replace")
                        logger.debug(f"ZMQ Response from {reply_server}: {reply_text}")
                    else:
                        # ZMQ timeout
                        logger.warning("ZMQ Timed out waiting for reply")

                    zmq_socket.close()

                    end_time = time.perf_counter()
                    latency = (end_time - start_time) * 1000000  # microsec

                except Exception as e:
                    logger.error(f"Exception while sending message with ZMQ: {e}")
                    zmq_socket.close()
                finally:
                    return latency

        except ImportError as e:
            logger.error(f"Could not import zmq (ZeroMQ): {e}")
            zmq_context.term()
            return

    try:
        consumer = None
        try:
            consumer = ConsumerQueue(protocol, ipv4)
        except FileNotFoundError:
            # If the shared memory file is not found, simply return
            logger.error(
                f"[{protocol}:{ipv4}] Shared memory not found. Exiting thread."
            )
            return

        while not stop_event.is_set():
            msgs = consumer.read_messages()
            if msgs:
                for m in msgs:
                    logger.debug(f"[{protocol}:{ipv4}] Consumer gets message: {m}")
                    if protocol_to_report_result == "zmq":
                        send_latency = send_message_via_zmq(m, protocol, ipv4)
                    else:
                        send_latency = send_message_via_http(
                            m,
                            f"/result_{protocol}",
                            control_host,
                            collect_port_http,
                            logger,
                        )

                    # if send latency is over 1 second, print error message
                    if send_latency > 1000000:
                        logger.warning(
                            f"[{protocol}:{ipv4}] Latency to report using {protocol_to_report_result}: {send_latency} us"
                        )

            else:
                # If there's no message, sleep for 10 milliseconds
                time.sleep(0.01)

    except KeyboardInterrupt:
        logger.info("Consumer interrupted. Exiting...")
    except Exception as e:
        logger.info(
            f"Unexpectedly terminate message process thread: {protocol}_{ipv4}: {e}"
        )
    finally:
        # garbage collection
        if protocol_to_report_result == "zmq":
            zmq_context.term()


def agent_sender():
    """
    Periodically checks the SHMEM_DIR for any new shared memory files.
    If found, it creates a separate thread to execute 'thread_process_messages'.
    """
    files_running = dict()  # key: filename, value: Thread object

    while True:
        if os.path.isdir(SHMEM_DIR):
            file_list = [
                fname
                for fname in os.listdir(SHMEM_DIR)
                if os.path.isfile(os.path.join(SHMEM_DIR, fname))
            ]
        else:
            logger.warning(f"{SHMEM_DIR} directory does not exist.")
            file_list = []

        # (1) Check if any existing threads have died, and remove them from 'files_running'
        dead_keys = []
        for fname, t_dict in files_running.items():
            thread_obj = t_dict["thread"]
            stop_event = t_dict["stop_event"]

            # Check if thread is no longer alive
            if not thread_obj.is_alive():
                logger.info(
                    f"Thread for {fname} is no longer alive. Scheduling removal."
                )
                dead_keys.append(fname)

            # Check IP is still active
            protocol, ipv4 = fname.strip().split("_")
            if not check_ip_active(ipv4, logger):
                stop_event.set()  # Signal the thread to stop

        # Now remove outside the loop
        for fname in dead_keys:
            del files_running[fname]

        # (2) For any newly discovered shared memory files, start a new thread if not already running
        for fname in file_list:
            # The shared memory filename is assumed to have the format: "protocol"_"ipv4"
            try:
                # filter invalid filenames or protocol
                protocol, ipv4 = fname.strip().split("_")
                if (
                    not check_ip_active(ipv4, logger)
                    or protocol not in TARGET_PROTOCOLS
                ):
                    logger.debug(f"Unavailable: {fname}:{protocol}:{ipv4}. Skipping.")
                    continue
            except ValueError:
                logger.debug(f"Invalid shmem filename format: {fname}. Skipping.")
                continue
            except Exception as e:
                logger.error(f"Unexpected error parsing filename {fname}: {e}")
                sys.exit(1)

            if fname not in files_running:
                logger.info(f"Found new shmem {fname}. Starting a new thread.")
                stop_event = threading.Event()  # Create a stop signal
                t = threading.Thread(
                    target=thread_process_messages,
                    args=(protocol, ipv4, stop_event),
                    daemon=True,
                )
                files_running[fname] = {"thread": t, "stop_event": stop_event}
                t.start()

        # (3) Sleep for 1 second before checking again
        time.sleep(1)


def run_agent_sender():
    try:
        agent_sender()
    except KeyboardInterrupt:
        logger.info("agent_sender process received KeyboardInterrupt. Exiting.")
