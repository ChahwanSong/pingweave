#!/usr/bin/env python3
# -*- coding: utf-8 -*-

from common import *
from macro import *
from logger import initialize_pingweave_logger

logger = initialize_pingweave_logger(socket.gethostname(), "main", 10, True)

name_controller = "pingweave_ctrl"
name_agent = "pingweave_agent"
name_tcp_server = "tcp_server"
name_tcp_client = "tcp_client"
name_udp_server= "udp_server"
name_udp_client = "udp_client"
name_roce_server = "roce_server"
name_roce_client = "roce_client"
name_ib_server = "ib_server"
name_ib_client = "ib_client"

protocol_to_process_names = {
    "tcp": [name_tcp_server, name_tcp_client],
    "udp": [name_udp_server, name_udp_client],
    "roce": [name_roce_server, name_roce_client],
    "ib": [name_ib_server, name_ib_client],
}


processes_to_run = {
    name_tcp_server: [f"{BIN_DIR}/pingweave_tcp_server"],
    name_tcp_client: [f"{BIN_DIR}/pingweave_tcp_client"],
    name_udp_server: [f"{BIN_DIR}/pingweave_udp_server"],
    name_udp_client: [f"{BIN_DIR}/pingweave_udp_client"],
    name_roce_server: [f"{BIN_DIR}/pingweave_rdma_server"],
    name_roce_client: [f"{BIN_DIR}/pingweave_rdma_client"],
    name_ib_server: [f"{BIN_DIR}/pingweave_rdma_server"],
    name_ib_client: [f"{BIN_DIR}/pingweave_rdma_client"],
    name_controller: [sys.executable, f"{SCRIPT_DIR}/pingweave_ctrl.py"],
    name_agent: [sys.executable, f"{SCRIPT_DIR}/pingweave_agent.py"],
}


def main():
    # main thread start alarm to controller
    latency = send_message_via_http(
        "Main starts",
        "/alarm",
        config["control_host"],
        config["collect_port_http"],
        logger,
    )
    logger.info(f"HTTP latency to controller: {latency}")

    # kill all running pingweave processes (pkill -f pingweave)
    kill_pingweave_except_main(logger)

    # clean directory - /download, /upload
    delete_files_in_directory(UPLOAD_PATH, logger)
    delete_files_in_directory(DOWNLOAD_PATH, logger)

    # archive of processes
    running_processes = {}
    pinglist_yaml_load_retry_cntr = -1

    try:
        while True:
            # 0) TODO: handle zombie process
            start_time = time.perf_counter()

            # 1) read pingweave.ini to get control_host address
            try:
                config_parser.read(CONFIG_PATH)
                config["control_host"] = config_parser["controller"]["host"]
            except Exception as e:
                msg = f"Error reading pingweave.ini: {e}"
                logger.critical(msg)
                send_message_via_http(msg, "/alarm", config["control_host"], config["collect_port_http"], logger)

            # 1) get local ip address
            local_ips = set()
            for iface, ip in get_interfaces().items():
                if iface == "lo" or "virbr" in iface or "docker" in iface:
                    continue
                # add to local ips
                local_ips.add(ip)

            # 2) get my ping-valid ip address from pinglist.yaml
            if config["control_host"] in local_ips:  # if agent
                pinglist_path = PINGLIST_PATH
            else:
                pinglist_path = os.path.join(DOWNLOAD_PATH, "pinglist.yaml")

            # mydict = {"tcp": ["10.0.0.1",], "udp": ...}
            myaddr = get_my_addr_from_pinglist(pinglist_path, local_ips, logger)
            if myaddr:
                # successfully load the pinglist.yaml
                pinglist_yaml_load_retry_cntr = 0 # reset
                for protocol, ip_list in myaddr.items():
                    logger.debug(
                        "Size of {} in pinglist.yaml: {}", protocol, len(ip_list)
                    )
            else:
                # failed to load the pinglist.yaml
                if (
                    pinglist_yaml_load_retry_cntr >= 0
                    and pinglist_yaml_load_retry_cntr < 10
                ):
                    logger.warning(
                        "Loading a pinglist is failed. Retry count: {}/10",
                        pinglist_yaml_load_retry_cntr,
                    )
                    time.sleep(1)
                    continue
            
            
            # 3) terminate processes which are no more in pinglist
            key_to_pop = []
            msgs_to_send = []
            for key, proc in running_processes.items():
                k_ip = key[0]
                k_pname = key[1]
                k_protocol = key[2]
                
                # default message
                msg = f"{k_ip}:{k_pname} Process is terminated (unknown)."

                if proc is None or proc.poll() is not None:
                    key_to_pop.append(key)
                    continue

                # ignore pingweave_agent.py - we never terminate it
                if k_pname == name_agent:
                    continue
                
                # check the change of "control_host" field in pingweave.ini
                if k_pname == name_controller:
                    if k_ip != config["control_host"]:
                        proc.kill()
                        msg = f"{k_ip}:{k_pname} is terminated: 'control_host' field in pingweave.ini might be changed."
                        msgs_to_send.append(msg)
                    
                    continue

                # if IP is no more in pinglist or the interface is no more active, remove
                ips = myaddr[k_protocol]
                if k_ip not in ips:
                    proc.kill()
                    msg = f"{k_ip}:{k_pname} is terminated: no more in pinglist."
                    msgs_to_send.append(msg)
            
                if k_ip not in local_ips:
                    proc.kill()
                    msg = f"{k_ip}:{k_pname} is terminated: no more active interface."
                    msgs_to_send.append(msg)
                
            # send alarm messages
            for msg in msgs_to_send:
                logger.info(msg)
                send_message_via_http(msg, "/alarm", config["control_host"], config["collect_port_http"], logger)
                
            # clean up
            for key in key_to_pop:
                running_processes.pop(key)


            # 4) start new processes
            #   -> pingweave_ctrl.py (if controller node)
            if config["control_host"] in local_ips:
                # key : (ip, process_name)
                key = (config["control_host"], name_controller, "control")
                if key not in running_processes or not (running_processes[key] and running_processes[key].poll() is None):
                    f_to_run = processes_to_run[key[1]]
                    running_processes[key] = start_process(f_to_run, key[1], logger)
                    time.sleep(1)
                    send_message_via_http(f"{key[1]} starts", "/alarm", config["control_host"], config["collect_port_http"], logger)

            #   -> pingweave_agent.py (always running)
            # key : (ip, process_name)
            key = ("agent", name_agent, "control")
            if key not in running_processes or not (running_processes[key] and running_processes[key].poll() is None):
                f_to_run = processes_to_run[key[1]]
                running_processes[key] = start_process(f_to_run, key[1], logger)
                send_message_via_http(f"{key[1]} starts", "/alarm", config["control_host"], config["collect_port_http"], logger)
        
            #   -> cpp programs
            if myaddr:
                for protocol, ips in myaddr.items():
                    for ip in ips:
                        if protocol in protocol_to_process_names:
                            for pname in protocol_to_process_names[protocol]:
                                # key : (ip, process_name, protocol)
                                key = (ip, pname, protocol)
                                if key not in running_processes or not (running_processes[key] and running_processes[key].poll() is None):
                                    f_to_run = processes_to_run[key[1]] + [ip]
                                    if key[2] in ["roce", "ib"]:
                                        f_to_run = f_to_run + [key[2]]
                                    running_processes[key] = start_process(f_to_run, key[1], logger)
                                    send_message_via_http(f"{key[1]} starts", "/alarm", config["control_host"], config["collect_port_http"], logger)
            
            end_time = time.perf_counter()
            elapsed_time = float(end_time - start_time) # second
            logger.debug(f"Elapsed to for a loop: {elapsed_time}")
            time.sleep(max(1 - elapsed_time, 0.1))
            pass
    except KeyboardInterrupt:
        logger.info(f"Keyboard Interruption. Graceful Shutdown!")
        sys.exit(0)

    except Exception as e:
        msg = f"** Main terminated: {e}"
        logger.info(f"** Main terminated: {e}")
        send_message_via_http(msg, "/alarm", config["control_host"], config["collect_port_http"], logger)
        # If needed, you can add logic here to terminate all child processes
        sys.exit(0)
    finally:
        for k, p in running_processes.items():
            p.kill()

if __name__ == "__main__":
    main()
