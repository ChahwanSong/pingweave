import redis  # in-memory key-value storage
from datetime import datetime, timedelta
import pandas as pd
import copy
import numpy as np
import socket
import psutil
import asyncio
import yaml
import plotly.graph_objects as go
from logger import initialize_pingweave_logger
import os
import time
import configparser
from macro import *

logger = initialize_pingweave_logger(socket.gethostname(), "plotter", 5, False)

# Global variables
control_host = None
collect_port = None
interval_report_ping_result_millisec = None
colorscale = ["black", "purple", "green", "yellow", "orange", "red"]

# Variables to save pinglist
pinglist_in_memory = {}

# ConfigParser object
config = configparser.ConfigParser()


# value to color index mapping for ping results
def map_value_to_color_index_ping_delay(value, steps: list):
    assert(len(steps) == 3)
    if value <= -1:
        return 0  # black
    elif -1 < value <= 0:
        return 1  # purple
    elif 0 < value <= int(steps[0]):
        return 2  # green
    elif int(steps[0]) < value <= int(steps[1]):
        return 3  # yellow
    elif int(steps[1]) < value <= int(steps[2]):
        return 4  # orange
    elif value > int(steps[2]):
        return 5  # red
    else:
        logger.error(f"map_value error: {steps}")
        exit(1)


# value to color index mapping for ping results
def map_value_to_color_index_success_ratio(value, steps: list):
    assert(len(steps) == 3)
    if value < -1:
        return 0  # black
    elif -1 <= value < 0:
        return 1  # purple
    elif 0 <= value < float(steps[0]):
        return 2 # red
    elif float(steps[0]) <= value < float(steps[1]):
        return 3 # orange
    elif float(steps[1]) <= value < float(steps[2]):
        return 4 # yellow
    elif float(steps[2]) <= value <= 1:
        return 5 # green
    else:
        logger.error(f"map_value error: {steps}")
        exit(1)


# global logics
try:
    # Redis
    socket_path = "/var/run/redis/redis-server.sock"
    redis_server = redis.StrictRedis(
        unix_socket_path=socket_path, decode_responses=True
    )
    logger.info(f"Redis server running - {redis_server.ping()}")  # 출력: True
    assert redis_server.ping()
except redis.exceptions.ConnectionError as e:
    logger.error(f"Cannot connect to Redis server: {e}")
    if not os.path.exists(socket_path):
        logger.error(f"Socket file does not exist: {socket_path}")
    redis_server = None
except FileNotFoundError as e:
    logger.error(f"Redis socket file does not exist: {e}")
    redis_server = None
except Exception as e:
    logger.error(f"Unexpected error of Redis server: {e}")
    redis_server = None


def load_config_ini():
    global control_host, collect_port, interval_report_ping_result_millisec

    try:
        config.read(CONFIG_PATH)
        control_host = config["controller"]["host"]
        collect_port = int(config["controller"]["port_collect"])
        interval_report_ping_result_millisec = int(
            config["param"]["interval_report_ping_result_millisec"]
        )
        logger.debug("Configuration loaded successfully from config file.")
    except Exception as e:
        logger.error(f"Error reading configuration: {e}")
        control_host = None
        collect_port = None


def check_ip_active(target_ip):
    """
    Checks if the given IP address is active and associated with an interface that is UP.
    """
    try:
        net_if_addrs = psutil.net_if_addrs()
        net_if_stats = psutil.net_if_stats()

        for iface, addrs in net_if_addrs.items():
            for addr in addrs:
                if addr.family == socket.AF_INET and addr.address == target_ip:
                    if iface in net_if_stats and net_if_stats[iface].isup:
                        return True
                    else:
                        logger.error(f"Interface {iface} with IP {target_ip} is down.")
                        return False
        logger.error(f"No active interface found with IP address {target_ip}.")
        return False
    except Exception as e:
        logger.error(f"Error checking IP activity: {e}")
        return False


def read_pinglist():
    global pinglist_in_memory

    try:
        pinglist_in_memory.clear()
        if os.path.isfile(PINGLIST_PATH):
            with open(PINGLIST_PATH, "r") as file:
                pinglist_in_memory = yaml.safe_load(file)
                logger.debug("Pinglist loaded successfully.")
        else:
            logger.error(f"Pinglist file not found at {PINGLIST_PATH}")
    except Exception as e:
        logger.error(f"Error loading pinglist: {e}")

def clear_directory_conditional(directory_path: str, except_files: list):
    try:
        # get entries in directory
        for entry in os.listdir(directory_path):
            entry_path = os.path.join(directory_path, entry)

            # check except files
            entry_prefix = entry.split(".html")[0]
            if entry_prefix not in except_files:
                logger.info(f"Deleting a file or directory: {entry}")
                
                # delete if file 
                if os.path.isfile(entry_path) or os.path.islink(entry_path):
                    os.remove(entry_path)
                    logger.info(f"Deleted file: {entry_path}")

                # delete if sub-directory
                elif os.path.isdir(entry_path):
                    for root, dirs, files in os.walk(entry_path, topdown=False):
                        for file in files:
                            file_path = os.path.join(root, file)
                            if os.path.basename(file_path) not in except_files:
                                os.remove(file_path)
                                logger.info(f"Deleted file: {file_path}")
                        for dir_name in dirs:
                            dir_path = os.path.join(root, dir_name)
                            os.rmdir(dir_path)
                            logger.info(f"Deleted directory: {dir_path}")
                    os.rmdir(entry_path)
                    logger.info(f"Deleted directory: {entry_path}")

    except Exception as e:
        logger.error(f"An error occurred: {e}")



def plot_heatmap_value(
    records: list,
    value_name: str,
    time_name: str,
    steps: list,
    tick_steps: list,
    map_func,
    outname: str,
):
    try:
        # sanity check
        if len(tick_steps) != len(colorscale):
            logger.error(
                f"Length of tick_steps must be same with that of colorscale: {tick_steps} vs {colorscale}"
            )
            raise RuntimeError("plotter tick_steps and colorscale mismatch")

        # dataframe
        df = pd.DataFrame(records)

        # index mapping to ip address
        source_ips = df["source"].unique()
        destination_ips = df["destination"].unique()
        source_ip_to_index = {ip: idx for idx, ip in enumerate(source_ips)}
        destination_ip_to_index = {ip: idx for idx, ip in enumerate(destination_ips)}
        df["source_idx"] = df["source"].map(source_ip_to_index)
        df["destination_idx"] = df["destination"].map(destination_ip_to_index)

        # create pivot table
        pivot_table = df.pivot(
            index="destination_idx", columns="source_idx", values=value_name
        ).fillna(-1)
        z_values = pivot_table.values

        # time-series pivot table
        time_pivot = df.pivot(
            index="destination_idx", columns="source_idx", values=time_name
        ).fillna("N/A")
        time_values = time_pivot.values

        # create text matrix
        text_matrix = []
        for i in range(z_values.shape[0]):
            row = []
            for j in range(z_values.shape[1]):
                src = source_ips[j]
                dst = destination_ips[i]
                val = z_values[i][j]
                time = time_values[i][j]
                text = f"Src: {src}<br>Dst: {dst}<br>Value: {val}<br>Time: {time}"
                row.append(text)
            text_matrix.append(row)

        # function vectorize
        vectorized_map_func = np.vectorize(lambda x: map_func(x, steps))
        z_colors = vectorized_map_func(z_values)

        # cell number calc
        num_x = len(pivot_table.columns)
        num_y = len(pivot_table.index)

        # dynamic xgap and ygap
        xgap = max(1, int(20 / num_x))
        ygap = max(1, int(20 / num_y))

        # create a heatmap
        fig = go.Figure(
            data=go.Heatmap(
                z=z_colors,
                colorscale=colorscale,
                x=pivot_table.columns.values,
                y=pivot_table.index.values,
                zmin=0,  # setting min
                zmax=len(tick_steps) - 1,  # setting max
                xgap=xgap,  # dynamic horizontal space
                ygap=ygap,  # dynamic vertical space
                customdata=text_matrix,  # customdata <- text matrix
                hovertemplate="%{customdata}",  # mouse cursor
                hoverinfo="text",  # hover info
                name="",  # empty trace name
                colorbar=dict(
                    tickmode="array",
                    tickvals=list(range(len(tick_steps))),
                    ticktext=tick_steps,
                    title=value_name,
                ),
            )
        )

        current_time = datetime.now().strftime("%Y-%m-%d %H:%M:%S")

        # update layout
        fig.update_layout(
            xaxis_title="Source IP",
            yaxis_title="Destination IP",
            title=f"{outname} ({current_time})",
            plot_bgcolor="white",
            paper_bgcolor="white",
        )

        # hide an axis label
        fig.update_xaxes(visible=True)
        fig.update_yaxes(visible=True)

        # save to HTML file
        fig.write_html(f"{HTML_DIR}/{outname}.html")

        # return the path of HTML
        return f"{HTML_DIR}/{outname}.html"
    
    except Exception as e:
        logger.error(f"Exception: {e}")
        return "" # return nothing if failure

def plot_heatmap_udp(data, outname="result"):
    delay_steps = [500000, 2000000, 10000000]
    ratio_steps = [0.0, 0.3, 0.6]
    delay_tick_steps = ["No Data", "Failure", "~500µs", "~2ms", "~10ms", ">10ms"]
    ratio_tick_steps = ["No Data", "Failure", "0%", "30%", "60%", "100%"]
    
    output_files = []
    records = []
    for k, v in data.items():
        src, dst = k.split(",")
        if v:
            ts_ping_start, ts_ping_end, n_success, n_failure = v[0:4]
            n_success = int(n_success)
            n_failure = int(n_failure)
            total_attempts = n_success + n_failure
            if total_attempts == 0:
                success_ratio = -1
            else:
                success_ratio = 1.0 * n_success / total_attempts

            _, network_mean, network_max, network_p50, network_p95, network_p99 = v[
                4:10
            ]
            records.append(
                {
                    "source": src,
                    "destination": dst,
                    "success_ratio": success_ratio,
                    "network_mean": float(network_mean),
                    "network_p50": float(network_p50),
                    "network_p99": float(network_p99),
                    "ping_start_time": ts_ping_start,
                    "ping_end_time": ts_ping_end,
                }
            )
        else:
            records.append(
                {
                    "source": src,
                    "destination": dst,
                    "success_ratio": 0.0,
                    "network_mean": -1,
                    "network_p50": -1,
                    "network_p99": -1,
                    "ping_start_time": "N/A",
                    "ping_end_time": "N/A",
                }
            )
    # network mean
    if plot_heatmap_value(
        records,
        "network_mean",
        "ping_end_time",
        delay_steps,
        delay_tick_steps,
        map_value_to_color_index_ping_delay,
        outname + "_network_mean",
    ):
        output_files.append(outname + "_network_mean")
    # network p50
    if plot_heatmap_value(
        records,
        "network_p50",
        "ping_end_time",
        delay_steps,
        delay_tick_steps,
        map_value_to_color_index_ping_delay,
        outname + "_network_p50",
    ):
        output_files.append(outname + "_network_p50")
    # network p99
    if plot_heatmap_value(
        records,
        "network_p99",
        "ping_end_time",
        delay_steps,
        delay_tick_steps,
        map_value_to_color_index_ping_delay,
        outname + "_network_p99",
    ):
        output_files.append(outname + "_network_p99")
    # success ratio
    if plot_heatmap_value(
        records,
        "success_ratio",
        "ping_end_time",
        ratio_steps,
        ratio_tick_steps,
        map_value_to_color_index_success_ratio,
        outname + "_success_ratio",
    ):
        output_files.append(outname + "_success_ratio")
    return output_files

def plot_heatmap_rdma(data, outname="result"):
    delay_steps = [100000, 500000, 5000000]
    ratio_steps = [0, 0.3, 0.6]
    delay_tick_steps = ["No Data", "Failure", "~100µs", "~500µs", "~5ms", ">5ms"]
    ratio_tick_steps = ["No Data", "Failure", "100%", "90%", "50%", "0%"]

    output_files = []
    records = []
    for k, v in data.items():
        src, dst = k.split(",")
        if v:
            ts_ping_start, ts_ping_end, n_success, n_failure = v[0:4]
            n_success = int(n_success)
            n_failure = int(n_failure)
            total_attempts = n_success + n_failure
            if total_attempts == 0:
                success_ratio = 0.0
            else:
                success_ratio = 1.0 * n_success / total_attempts

            _, client_mean, client_max, client_p50, client_p95, client_p99 = v[4:10]
            _, network_mean, network_max, network_p50, network_p95, network_p99 = v[
                10:16
            ]
            _, server_mean, server_max, server_p50, server_p95, server_p99 = v[16:22]
            records.append(
                {
                    "source": src,
                    "destination": dst,
                    "success_ratio": success_ratio,
                    "network_mean": float(network_mean),
                    "client_mean": float(client_mean),
                    "server_mean": float(server_mean),
                    "network_p50": float(network_p50),
                    "client_p50": float(client_p50),
                    "server_p50": float(server_p50),
                    "network_p99": float(network_p99),
                    "client_p99": float(client_p99),
                    "server_p99": float(server_p99),
                    "ping_start_time": ts_ping_start,
                    "ping_end_time": ts_ping_end,
                }
            )
        else:
            records.append(
                {
                    "source": src,
                    "destination": dst,
                    "success_ratio": 0.0,
                    "network_mean": -1,
                    "client_mean": -1,
                    "server_mean": -1,
                    "network_p50": -1,
                    "client_p50": -1,
                    "server_p50": -1,
                    "network_p99": -1,
                    "client_p99": -1,
                    "server_p99": -1,
                    "ping_start_time": "N/A",
                    "ping_end_time": "N/A",
                }
            )
    # network mean
    if plot_heatmap_value(
        records,
        "network_mean",
        "ping_end_time",
        delay_steps,
        delay_tick_steps,
        map_value_to_color_index_ping_delay,
        outname + "_network_mean",
    ):
        output_files.append(outname + "_network_mean")
    # network p50
    if plot_heatmap_value(
        records,
        "network_p50",
        "ping_end_time",
        delay_steps,
        delay_tick_steps,
        map_value_to_color_index_ping_delay,
        outname + "_network_p50",
    ):
        output_files.append(outname + "_network_p50")
    # network p99
    if plot_heatmap_value(
        records,
        "network_p99",
        "ping_end_time",
        delay_steps,
        delay_tick_steps,
        map_value_to_color_index_ping_delay,
        outname + "_network_p99",
    ):
        output_files.append(outname + "_network_p99")
    # success ratio
    if plot_heatmap_value(
        records,
        "success_ratio",
        "ping_end_time",
        ratio_steps,
        ratio_tick_steps,
        map_value_to_color_index_success_ratio,
        outname + "_success_ratio",
    ):
        output_files.append(outname + "_success_ratio")    
    return output_files 


async def pingweave_plotter():
    load_config_ini()
    last_plot_time = int(time.time())
    pinglist_protocol = ["udp", "rdma"]

    try:
        while True:
            if not check_ip_active(control_host):
                logger.warning(
                    f"No active interface for controller host {control_host}. Exit..."
                )
                exit(1)

            try:
                # plot the graph for every X seconds
                now = int(time.time())
                if (
                    last_plot_time + int(interval_report_ping_result_millisec / 1000)
                    < now
                    and redis_server != None
                ):
                    # update the last plot time
                    last_plot_time = now

                    logger.info(
                        f"Pingweave plotter is running on {control_host}:{collect_port}"
                    )

                    # read pinglist (synchronous) -> pinglist_in_memory
                    # pinglist = {'udp': {'group1': ['192.168.1.1', '192.168.1.2', '192.168.1.3']}}
                    read_pinglist()

                    # then, create template records
                    records = copy.deepcopy(pinglist_in_memory)
                    map_ip_to_groups = {}  # dict of set
                    for proto, cat_data in pinglist_in_memory.items():
                        for group, ip_list in cat_data.items():
                            # make a template
                            records[proto][group] = {
                                f"{src},{dst}": None
                                for src in ip_list
                                for dst in ip_list
                            }

                            # make a mapping
                            for ip in ip_list:
                                if ip not in map_ip_to_groups:
                                    map_ip_to_groups[ip] = set()
                                map_ip_to_groups[ip].add(group)

                    # Read data from redis in-memory storage.
                    cursor = "0"
                    while cursor != 0:
                        current_time = datetime.now()
                        cursor, keys = redis_server.scan(cursor=cursor)  # scan kv-store
                        for key in keys:
                            value = redis_server.get(key)
                            if value is None:
                                logger.warning(
                                    f"Redis Key {key} not found in Redis. Skipping..."
                                )
                                continue

                            value = value.split(",")

                            # filter old information
                            measure_time = datetime.strptime(
                                value[1][:26], "%Y-%m-%d %H:%M:%S.%f"
                            )
                            # caclulate a time difference
                            time_difference = abs(
                                (current_time - measure_time).total_seconds()
                            )

                            # skip if the info is stale
                            if time_difference > INTERVAL_PLOTTER_FILTER_OLD_DATA_SEC:
                                logger.debug(
                                    f"Ignore old data: {key} | Time: {measure_time} | T_Diff: {time_difference}"
                                )
                                continue

                            proto, src, dst = key.split(",")
                            record_key = f"{src},{dst}"
                            if proto not in pinglist_protocol:
                                raise Exception(f"Not expected protocol type: {proto}")

                            # check which groups to insert
                            src_groups = map_ip_to_groups[src]
                            dst_groups = map_ip_to_groups[dst]
                            common_groups = list(set(src_groups) & set(dst_groups))

                            # insertion
                            for group in common_groups:
                                if group in records[proto]:
                                    if record_key not in records[proto][group]:
                                        raise Exception(
                                            f"{record_key} is not in records[{proto}][{group}]"
                                        )
                                    records[proto][group][record_key] = value

                    # plot for each category/group
                    new_file_list = []
                    for category, data in records.items():
                        for group, group_data in data.items():
                            if category == "udp":
                                new_file_list += plot_heatmap_udp(group_data, f"{category}_{group}")
                            elif category == "rdma":
                                new_file_list += plot_heatmap_rdma(group_data, f"{category}_{group}")
                    
                    # clear all HTML
                    clear_directory_conditional(HTML_DIR, new_file_list)

            except KeyError as e:
                logger.error(f"Plotter - Missing key error: {e}")
            except TypeError as e:
                logger.error(f"Plotter - Type error encountered: {e}")
            except IndexError as e:
                logger.error(f"Plotter -  IndexError occurred: {e}")
            except Exception as e:
                logger.error(f"Plotter - Unhandled exception: {e}")
            finally:
                await asyncio.sleep(1)
    except KeyboardInterrupt:
        logger.info("pingweave_plotter received KeyboardInterrupt. Exiting.")
    except Exception as e:
        logger.error(f"Exception in pingweave_plotter: {e}")


def run_pingweave_plotter():
    try:
        asyncio.run(pingweave_plotter())
    except KeyboardInterrupt:
        logger.info("pingweave_plotter process received KeyboardInterrupt. Exiting.")
