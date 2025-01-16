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
from setproctitle import setproctitle
from concurrent.futures import ThreadPoolExecutor

from macro import *

tcpudp_delay_steps = [1000000, 5000000, 20000000]
tcpudp_delay_tick_steps = ["No Data", "Failure", "~1ms", "~5ms", "~20ms", ">20ms"]
tcpudp_ratio_steps = [0.05, 0.5, 0.9]
tcpudp_ratio_tick_steps = ["No Data", "Failure", "~5%", "~50%", "~90%", "All failed"]

rdma_delay_steps = [100000, 500000, 5000000]
rdma_delay_tick_steps = ["No Data", "Failure", "~100µs", "~500µs", "~5ms", ">5ms"]
rdma_ratio_steps = [0.05, 0.5, 0.9]
rdma_ratio_tick_steps = ["No Data", "Failure", "~5%", "~50%", "~90%", "All failed"]

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
    assert len(steps) == 3
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
def map_value_to_color_index_ratio(value, steps: list):
    assert len(steps) == 3
    if value <= -1:
        return 0  # black
    elif -1 < value < 0:
        return 1  # purple
    elif 0 <= value < float(steps[0]):
        return 2  # green
    elif float(steps[0]) <= value < float(steps[1]):
        return 3  # yellow
    elif float(steps[1]) <= value < float(steps[2]):
        return 4  # orange
    elif float(steps[2]) <= value <= 1:
        return 5  # red
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


def clear_directory_conditional(
    directory_path: str, except_files: list, format: str = "html"
):
    try:
        # get entries in directory
        for entry in os.listdir(directory_path):
            entry_path = os.path.join(directory_path, entry)

            # check except files
            entry_prefix = entry.split(f".{format}")[0]
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
                formatted_val = (
                    f"{int(val):,}" if val >= 1 else f"{val:.2f}" if val >= 0 else "N/A"
                )
                time = time_values[i][j]
                text = f"Src: {src}<br>Dst: {dst}<br>Value: {formatted_val}<br>Time: {time}"
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
            autosize=True,
            width=1080,
            height=950,
            plot_bgcolor="white",
            paper_bgcolor="white",
        )

        # hide an axis label
        fig.update_xaxes(visible=True, showticklabels=False)
        fig.update_yaxes(visible=True, showticklabels=False)

        # save to HTML file
        fig.write_html(f"{HTML_DIR}/{outname}.html")

        # save to IMAGE file
        fig.write_image(f"{IMAGE_DIR}/{outname}.png", scale=4, width=800, height=700)

        # return the path of HTML
        return f"{HTML_DIR}/{outname}.html"

    except Exception as e:
        logger.error(f"Exception: {e}")
        return ""  # return nothing if failure


def calculate_ratios(n_success, n_failure, n_weird):
    failure_ratio = (
        -1 if n_success + n_failure == 0 else n_failure / (n_success + n_failure)
    )
    weird_ratio = (
        -1
        if n_success + n_failure + n_weird == 0
        else n_weird / (n_success + n_failure + n_weird)
    )
    return failure_ratio, weird_ratio


def prepare_default_record(src, dst, plot_type):
    default_record = {
        "source": src,
        "destination": dst,
        "failure_ratio": -1,
        "weird_ratio": -1,
        "network_mean": -1,
        "ping_start_time": "N/A",
        "ping_end_time": "N/A",
    }
    if plot_type == "rdma":
        default_record.update(
            {
                "client_mean": -1,
                "server_mean": -1,
                "network_p50": -1,
                "client_p50": -1,
                "server_p50": -1,
                "network_p99": -1,
                "client_p99": -1,
                "server_p99": -1,
            }
        )
    else:
        default_record.update(
            {
                "network_p50": -1,
                "network_p99": -1,
            }
        )
    return default_record


def prepare_record(k, v, plot_type):
    src, dst = k.split(",")
    if not v:
        return prepare_default_record(src, dst, plot_type)

    ts_ping_start, ts_ping_end, n_success, n_failure, n_weird = v[:5]
    failure_ratio, weird_ratio = calculate_ratios(
        int(n_success), int(n_failure), int(n_weird)
    )

    if plot_type == "tcpudp":
        _, network_mean, _, network_p50, _, network_p99 = v[5:11]
        return {
            "source": src,
            "destination": dst,
            "failure_ratio": failure_ratio,
            "weird_ratio": weird_ratio,
            "network_mean": float(network_mean),
            "network_p50": float(network_p50),
            "network_p99": float(network_p99),
            "ping_start_time": ts_ping_start,
            "ping_end_time": ts_ping_end,
        }

    if plot_type == "rdma":
        _, client_mean, _, client_p50, _, client_p99 = v[5:11]
        _, network_mean, _, network_p50, _, network_p99 = v[11:17]
        _, server_mean, _, server_p50, _, server_p99 = v[17:23]
        return {
            "source": src,
            "destination": dst,
            "failure_ratio": failure_ratio,
            "weird_ratio": weird_ratio,
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


def plot_heatmap(data, category_group_name="group", plot_type="tcpudp"):
    output_files = []
    records = [prepare_record(k, v, plot_type) for k, v in data.items()]

    plot_params = {
        "tcpudp": [
            (
                "network_mean",
                tcpudp_delay_steps,
                tcpudp_delay_tick_steps,
                map_value_to_color_index_ping_delay,
            ),
            (
                "network_p50",
                tcpudp_delay_steps,
                tcpudp_delay_tick_steps,
                map_value_to_color_index_ping_delay,
            ),
            (
                "network_p99",
                tcpudp_delay_steps,
                tcpudp_delay_tick_steps,
                map_value_to_color_index_ping_delay,
            ),
            (
                "failure_ratio",
                tcpudp_ratio_steps,
                tcpudp_ratio_tick_steps,
                map_value_to_color_index_ratio,
            ),
            (
                "weird_ratio",
                tcpudp_ratio_steps,
                tcpudp_ratio_tick_steps,
                map_value_to_color_index_ratio,
            ),
        ],
        "rdma": [
            (
                "network_mean",
                rdma_delay_steps,
                rdma_delay_tick_steps,
                map_value_to_color_index_ping_delay,
            ),
            (
                "network_p50",
                rdma_delay_steps,
                rdma_delay_tick_steps,
                map_value_to_color_index_ping_delay,
            ),
            (
                "network_p99",
                rdma_delay_steps,
                rdma_delay_tick_steps,
                map_value_to_color_index_ping_delay,
            ),
            (
                "failure_ratio",
                rdma_ratio_steps,
                rdma_ratio_tick_steps,
                map_value_to_color_index_ratio,
            ),
            (
                "weird_ratio",
                rdma_ratio_steps,
                rdma_ratio_tick_steps,
                map_value_to_color_index_ratio,
            ),
        ],
    }[plot_type]

    def process_plot(metric, steps, tick_steps, color_index):
        plot_name = "{}_{}".format(category_group_name, metric)
        if plot_heatmap_value(
            records, metric, "ping_end_time", steps, tick_steps, color_index, plot_name
        ):
            return plot_name
        return None

    with ThreadPoolExecutor() as executor:
        futures = [
            executor.submit(process_plot, metric, steps, tick_steps, color_index)
            for metric, steps, tick_steps, color_index in plot_params
        ]
        for future in futures:
            result = future.result()
            if result:
                output_files.append(result)

    return output_files


def process_category_group(category, group, group_data):
    if category in {"udp", "tcp"}:
        return plot_heatmap(group_data, f"{category}_{group}", "tcpudp")
    elif category in {"roce", "ib"}:
        return plot_heatmap(group_data, f"{category}_{group}", "rdma")
    return []


async def pingweave_plotter():
    load_config_ini()
    last_plot_time = int(time.time())
    pinglist_protocol = ["udp", "tcp", "roce", "ib"]

    try:
        while True:
            try:
                # plot the graph for every X seconds
                now_plot_time = int(time.time())
                if (
                    last_plot_time + int(interval_report_ping_result_millisec / 1000)
                    < now_plot_time
                    and redis_server != None
                ):
                    # update the last plot time
                    last_plot_time = now_plot_time

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
                                logger.warning(f"Redis Key {key} not found. Skip.")
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
                    with ThreadPoolExecutor() as executor:
                        # Create tasks for all combinations of category and group
                        futures = [
                            executor.submit(
                                process_category_group, category, group, group_data
                            )
                            for category, data in records.items()
                            for group, group_data in data.items()
                        ]

                        # Collect results
                        for future in futures:
                            new_file_list += future.result()

                    # clear all stale HTML and images
                    clear_directory_conditional(HTML_DIR, new_file_list, "html") 
                    clear_directory_conditional(IMAGE_DIR, new_file_list, "png")

                    elapsed_time = time.time() - now_plot_time
                    logger.info(
                        f"Pingweave plotter is running on {control_host}:{collect_port} | Elapsed time: {elapsed_time:.2f} seconds"
                    )
                else:
                    await asyncio.sleep(1)
            except KeyError as e:
                logger.error(f"Plotter - Missing key error: {e}")
            except TypeError as e:
                logger.error(f"Plotter - Type error encountered: {e}")
            except IndexError as e:
                logger.error(f"Plotter -  IndexError occurred: {e}")
            except Exception as e:
                logger.error(f"Plotter - Unhandled exception: {e}")
                
    except KeyboardInterrupt:
        logger.info("pingweave_plotter received KeyboardInterrupt. Exiting.")
    except Exception as e:
        logger.error(f"Exception in pingweave_plotter: {e}")


def run_pingweave_plotter():
    setproctitle("pingweave_plotter.py")
    try:
        asyncio.run(pingweave_plotter())
    except KeyboardInterrupt:
        logger.info("pingweave_plotter process received KeyboardInterrupt. Exiting.")
