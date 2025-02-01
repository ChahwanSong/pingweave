# import psutil
import socket
import ipaddress
import fcntl
import struct
import multiprocessing

target_protocols = ["tcp", "udp", "roce", "ib"]


def get_interfaces():
    """
    Retrieves all network interfaces and their corresponding IP addresses.
    """
    interfaces = {}
    try:
        with open("/proc/net/dev") as f:
            lines = f.readlines()[2:]  # Skip the header
            for line in lines:
                iface = line.split(":")[0].strip()
                ip = get_ip_address(iface)
                if ip:
                    interfaces[iface] = ip
    except FileNotFoundError:
        pass  # Not available on some systems
    return interfaces


def get_ip_address(iface):
    """
    Retrieves the IPv4 address of a given network interface.
    """
    try:
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        ip = fcntl.ioctl(
            sock.fileno(),
            0x8915,  # SIOCGIFADDR
            struct.pack("256s", iface[:15].encode("utf-8"))
        )[20:24]
        return socket.inet_ntoa(ip)
    except OSError:
        return None


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

# def check_ip_active(target_ip, logger):
#     """
#     Checks if the given IP address is:
#       1) A valid IPv4 address format.
#       2) Active and associated with an interface that is UP.
#     """

#     try:
#         # 1) Validate IPv4 format
#         try:
#             ip_obj = ipaddress.ip_address(target_ip)
#             if ip_obj.version != 4:
#                 logger.error(f"Invalid IPv4 address format: {target_ip}")
#                 return False
#         except ValueError:
#             logger.error(f"Invalid IPv4 address format: {target_ip}")
#             return False

#         # 2) Check if IP is active on an UP interface
#         net_if_addrs = psutil.net_if_addrs()
#         net_if_stats = psutil.net_if_stats()

#         for iface, addrs in net_if_addrs.items():
#             for addr in addrs:
#                 if addr.family == socket.AF_INET and addr.address == target_ip:
#                     if iface in net_if_stats and net_if_stats[iface].isup:
#                         return True
#                     else:
#                         logger.error(f"Interface {iface} with IP {target_ip} is down.")
#                         return False

#         logger.error(f"No active interface found with IP address {target_ip}.")
#         return False

#     except Exception as e:
#         logger.error(f"Error checking IP activity: {e}")
#         return False


def terminate_all(processes, logger):
    """
    Terminates all running processes gracefully.
    """
    for process in processes:
        if process.is_alive():
            process.terminate()
            logger.warning(f"Terminated process: {process.name}")

    for process in processes:
        process.join()
