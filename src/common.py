#!/usr/bin/env python3
# -*- coding: utf-8 -*-

# import psutil
import socket
import ipaddress
import fcntl
import struct
import copy
import subprocess
import time
import http.client
import stat
import os
import configparser
import sys
import multiprocessing
import signal
import logging
import atexit
import yaml

from macro import *

# ConfigParser object
config_parser = configparser.ConfigParser()

try:
    config_parser.read(CONFIG_PATH)
except Exception as e:
    logging.critical(f"Error reading pingweave.ini: {e}")
    exit(1)

# Global variables
try:
    config = {
        "control_host": config_parser["controller"]["host"],
        "control_port": int(config_parser["controller"]["control_port"]),
        "collect_port_http": int(config_parser["controller"]["collect_port_http"]),
        "collect_port_zmq": int(config_parser["controller"]["collect_port_zmq"]),
        "interval_sync_pinglist_sec": int(
            config_parser["param"]["interval_sync_pinglist_sec"]
        ),
        "interval_read_pinglist_sec": int(
            config_parser["param"]["interval_read_pinglist_sec"]
        ),
        "interval_report_ping_result_millisec": int(
            config_parser["param"]["interval_report_ping_result_millisec"]
        ),
        "protocol_to_report_result": config_parser["param"]["protocol_to_report_result"],
    }
except Exception as e:
    logging.critical(f"Error parsing pingweave.ini: {e}")
    exit(1)

def is_interface_up(iface):
    """
    Checks if the given network interface is up using SIOCGIFFLAGS ioctl.
    """
    try:
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        # Prepare the interface name in the required format
        ifreq = struct.pack("256s", iface[:15].encode("utf-8"))
        # SIOCGIFFLAGS: 0x8913 retrieves the interface flags
        res = fcntl.ioctl(sock.fileno(), 0x8913, ifreq)
        # The flags are stored in the bytes 16-18 of the result.
        flags, = struct.unpack("H", res[16:18])
        IFF_UP = 0x1  # Flag for interface being up (from net/if.h)
        return (flags & IFF_UP) == IFF_UP
    except Exception:
        return False

def get_ip_address(iface):
    """
    Retrieves the IPv4 address of a given network interface.
    """
    try:
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        # SIOCGIFADDR: 0x8915 retrieves the interface address
        ip = fcntl.ioctl(
            sock.fileno(),
            0x8915,
            struct.pack("256s", iface[:15].encode("utf-8")),
        )[20:24]
        return socket.inet_ntoa(ip)
    except OSError:
        return None


def get_interfaces():
    """
    Retrieves all UP network interfaces and their corresponding IP addresses.
    """
    interfaces = {}
    try:
        with open("/proc/net/dev") as f:
            lines = f.readlines()[2:]  # Skip the header lines
    
        if lines:
            for line in lines:
                iface = line.split(":")[0].strip()
                
                # Skip the local, virtual, and docker interfaces
                if iface == "lo" or "virbr" in iface or "docker" in iface:
                    continue

                # Only consider interfaces that are up
                if is_interface_up(iface):
                    ip = get_ip_address(iface)
                    if ip:
                        interfaces[iface] = ip
    except FileNotFoundError:
        pass  # Not available on some systems
    return interfaces


def check_ip_active(target_ip, logger):
    """
    Checks if the given IP address is:
      1) A valid IPv4 address format.
      2) Active and associated with an interface that is UP.
    """
    try:
        # 1) Validate IPv4 format
        try:
            ip_obj = ipaddress.ip_address(target_ip)
            if ip_obj.version != 4:
                logger.error(f"Invalid IPv4 address format: {target_ip}")
                return False
        except ValueError:
            logger.error(f"Invalid IPv4 address format: {target_ip}")
            return False

        # 2) Check if IP is active on an interface
        active_interfaces = get_interfaces()
        for iface, ip in active_interfaces.items():
            if ip == target_ip:
                return True

        logger.error(f"No active interface found with IP address {target_ip}.")
        return False

    except Exception as e:
        logger.error(f"Error checking IP activity: {e}")
        return False


# for multiprocess
def terminate_multiprocesses(processes, logger):
    """
    Terminates all running processes gracefully.
    """
    for process in processes:
        if process.is_alive():
            process.terminate()
            logger.warning(f"Terminated process: {process.name}")

    for process in processes:
        process.join()

# for subprocesses
def terminate_subprocesses(processes, logger):
    """
    Kills all child processes by sending a SIGTERM to their process groups.
    
    This function is registered with atexit to ensure cleanup is performed when the
    main process exits.
    """
    for process in processes:
        try:
            # Get the process group id of the child process
            pgid = os.getpgid(process.pid)
            logger.info(f"Killing process group with PGID: {pgid}")
            # Kill the entire process group
            os.killpg(pgid, signal.SIGTERM)
        except Exception as e:
            logger.error(f"Error killing process {process.pid}: {e}")


# for subprocess.Popen
def start_process(cmd_list, name, logger):
    """
    Start new process for a given cmd list, and return Popen object.
    """
    try:
        proc = subprocess.Popen(cmd_list)
        logger.info(f"{name} started (PID: {proc.pid})")
        return proc
    except Exception as e:
        logger.error(f"Failed to start {name}: {e}")
        return None


def kill_pingweave_except_main(logger):
    """
    Executes the 'pkill -f pingweave' command to terminate all processes
    that have 'pingweave' in their command line.
    """
    try:
        # Run the command and ensure it completes successfully
        subprocess.run(["pkill", "-f", "pingweave_"], check=False)
        logger.info("Successfully terminated existing processes...")
    except subprocess.CalledProcessError as error:
        logger.error("Error occurred while terminating processes:", error)
    except Exception as e:
        logger.error("Unexpected error when pkill -f pingweave: {e}")

def send_message_via_http(
    message: str, rest_api: str, control_host: str, collect_port_http: int, logger
):
    """Sends a POST request to the server with a timeout mechanism."""
    try:
        start_time = time.perf_counter()

        # Set timeout for the connection (3 seconds)
        socket.setdefaulttimeout(3)

        # Establish an HTTP connection
        conn = http.client.HTTPConnection(control_host, collect_port_http, timeout=3)

        # Headers
        headers = {"Content-Type": "text/plain", "Content-Length": str(len(message))}

        # Send POST request
        conn.request("POST", rest_api, body=message, headers=headers)

        # Get response
        response = conn.getresponse()
        response_text = response.read().decode("utf-8")
        logger.debug(f"HTTP Response [{response.status}]: {response_text}")

        end_time = time.perf_counter()
        latency = (end_time - start_time) * 1000000  # microsec

        # Close connection
        conn.close()

        return latency

    except socket.timeout:
        logger.error("Error: Request timed out!")
    except (http.client.HTTPException, ConnectionRefusedError) as e:
        logger.error(f"Error: Failed to send request - {e}")
    except Exception as e:
        logger.error(f"Unexpected error: {e}")


def delete_files_in_directory(directory_path, logger):
    """
    지정된 디렉토리 내의 모든 파일과 하위 디렉토리를 재귀적으로 삭제합니다.
    하위 디렉토리의 경우, 내부 파일들을 모두 삭제한 후 디렉토리 자체를 삭제합니다.
    """
    try:
        entries = os.listdir(directory_path)
    except Exception as e:
        logger.error(
            "Failed to open directory: {}. Error: {}".format(directory_path, e)
        )
        return

    for entry in entries:
        # "."와 ".."는 건너뛰기 (일반적으로 os.listdir()는 이 항목들을 포함하지 않음)
        if entry in (".", ".."):
            continue

        file_path = os.path.join(directory_path, entry)
        try:
            st = os.stat(file_path)
        except Exception as e:
            logger.error("Failed to stat file: {}. Error: {}".format(file_path, e))
            continue

        # 디렉토리인 경우 재귀 호출 후 디렉토리 삭제
        if stat.S_ISDIR(st.st_mode):
            delete_files_in_directory(file_path)
            try:
                os.rmdir(file_path)
                logger.info("Deleted directory: {}".format(file_path))
            except Exception as e:
                logger.error(
                    "Failed to remove directory: {}. Error: {}".format(file_path, e)
                )
        else:
            try:
                os.remove(file_path)
                logger.info("Deleted file: {}".format(file_path))
            except Exception as e:
                logger.error(
                    "Failed to remove file: {}. Error: {}".format(file_path, e)
                )

def get_my_addr_from_pinglist(pinglist_path: str, local_ips: set, logger):
    records = {k: set() for k in TARGET_PROTOCOLS}
    try:
        if os.path.isfile(pinglist_path):
            with open(pinglist_path, "r") as file:
                pinglist = yaml.safe_load(file)
                logger.debug(f"{pinglist_path} yaml was loaded successfully.")
            
            for protocol, group_data in pinglist.items():
                # filter invalid protocols
                if protocol not in records:
                    logger.debug(f"Skip to load invalid protocols: {protocol}")
                
                # get ip lists
                if not group_data:
                    continue
                
                for group, iplist in group_data.items():
                    if not iplist: 
                        continue
                    
                    for ip in iplist:
                        if ip in local_ips:
                            records[protocol].add(ip)
        else:
            logger.warning(f"Pinglist file not found at {pinglist_path}. Use empty pinglist.")
        
        return records
    except Exception as e:
        logger.warning("Failed to load pinglist.yaml")
        return False

    