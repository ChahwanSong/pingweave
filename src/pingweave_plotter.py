import os
import time
import socket
import configparser
import copy
import asyncio
import yaml
import redis  # in-memory key-value storage
import pandas as pd
import numpy as np
import psutil
import signal
import sys
from datetime import datetime, timedelta
from concurrent.futures import ProcessPoolExecutor
from setproctitle import setproctitle
import plotly.graph_objects as go
import matplotlib.pyplot as plt
import matplotlib.colors as mcolors

from macro import (
    CONFIG_PATH,
    INTERVAL_PLOTTER_FILTER_OLD_DATA_SEC,
    PINGLIST_PATH,
    HTML_DIR,
    IMAGE_DIR,
)
from logger import initialize_pingweave_logger

# ======================== #
#      Global Constants    #
# ======================== #

TCPUDP_DELAY_STEPS = [1_000_000, 5_000_000, 20_000_000]  # in ns
TCPUDP_DELAY_TICK_STEPS = ["No Data", "Failure", "~1ms", "~5ms", "~20ms", ">20ms"]
TCPUDP_RATIO_STEPS = [0.05, 0.5, 0.9]
TCPUDP_RATIO_TICK_STEPS = ["No Data", "Failure", "~5%", "~50%", "~90%", "All failed"]

RDMA_DELAY_STEPS = [100_000, 500_000, 5_000_000]  # in ns
RDMA_DELAY_TICK_STEPS = ["No Data", "Failure", "~100µs", "~500µs", "~5ms", ">5ms"]
RDMA_RATIO_STEPS = [0.05, 0.5, 0.9]
RDMA_RATIO_TICK_STEPS = ["No Data", "Failure", "~5%", "~50%", "~90%", "All failed"]

COLOR_SCALE = ["black", "purple", "green", "yellow", "orange", "red"]

logger = initialize_pingweave_logger(socket.gethostname(), "plotter", 5, False)

# These will be read from the config INI
control_host = None
collect_port = None
interval_report_ping_result_millisec = None

# Variables to store pinglist data
pinglist_in_memory = {}

# Shared ConfigParser object
config = configparser.ConfigParser()

# ======================== #
#        Redis Setup       #
# ======================== #

try:
    SOCKET_PATH = "/var/run/redis/redis-server.sock"
    redis_server = redis.StrictRedis(
        unix_socket_path=SOCKET_PATH, decode_responses=True
    )
    logger.info(f"Redis server running - {redis_server.ping()}")
    assert redis_server.ping()
except redis.exceptions.ConnectionError as e:
    logger.error(f"Cannot connect to Redis server: {e}")
    if not os.path.exists(SOCKET_PATH):
        logger.error(f"Socket file does not exist: {SOCKET_PATH}")
    redis_server = None
except FileNotFoundError as e:
    logger.error(f"Redis socket file does not exist: {e}")
    redis_server = None
except Exception as e:
    logger.error(f"Unexpected error connecting to Redis server: {e}")
    redis_server = None


# ======================== #
#     Config and Pinglist  #
# ======================== #


def load_config_ini() -> None:
    """
    Loads configuration from the CONFIG_PATH file and populates
    global variables: control_host, collect_port, interval_report_ping_result_millisec.
    """
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


def read_pinglist() -> None:
    """
    Reads the YAML pinglist from PINGLIST_PATH and updates the global
    dictionary pinglist_in_memory. If no file is found, logs an error.
    """
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


# ======================== #
#    Directory Management  #
# ======================== #


def clear_directory_conditional(
    directory_path: str, except_files: list[str], file_format: str = "html"
) -> None:
    """
    Removes files (or directories) within `directory_path` except those whose
    name (without extension) is in `except_files`. If subdirectories are found,
    they are also removed unless they contain an excepted file.
    """
    try:
        for entry in os.listdir(directory_path):
            entry_path = os.path.join(directory_path, entry)
            entry_prefix = entry.split(f".{file_format}")[0]

            # Skip files in the exception list
            if entry_prefix in except_files:
                continue

            logger.info(f"Deleting file or directory: {entry}")

            # Remove file or symlink
            if os.path.isfile(entry_path) or os.path.islink(entry_path):
                os.remove(entry_path)
                logger.info(f"Deleted file: {entry_path}")

            # Remove sub-directory
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
        logger.error(f"An error occurred while clearing directory: {e}")


# ======================== #
#       Value Mappers      #
# ======================== #


def map_value_to_color_index_ping_delay(value: float, steps: list[int]) -> int:
    """
    Maps a ping delay value (nanoseconds) to a color index based on the steps array.
    Returns an integer between 0 and 5 (inclusive).
    """
    assert len(steps) == 3, "Delay steps must have exactly 3 thresholds."
    if value <= -1:
        return 0  # black
    elif -1 < value <= 0:
        return 1  # purple
    elif 0 < value <= steps[0]:
        return 2  # green
    elif steps[0] < value <= steps[1]:
        return 3  # yellow
    elif steps[1] < value <= steps[2]:
        return 4  # orange
    elif value > steps[2]:
        return 5  # red
    raise ValueError(f"Unexpected delay value: {value}, Steps: {steps}")


def map_value_to_color_index_ratio(value: float, steps: list[float]) -> int:
    """
    Maps a ratio (e.g., failure ratio) to a color index based on the steps array.
    Returns an integer between 0 and 5 (inclusive).
    """
    assert len(steps) == 3, "Ratio steps must have exactly 3 thresholds."
    if value <= -1:
        return 0  # black
    elif -1 < value < 0:
        return 1  # purple
    elif 0 <= value < steps[0]:
        return 2  # green
    elif steps[0] <= value < steps[1]:
        return 3  # yellow
    elif steps[1] <= value < steps[2]:
        return 4  # orange
    elif steps[2] <= value <= 1:
        return 5  # red
    raise ValueError(f"Unexpected ratio value: {value}, Steps: {steps}")


# ======================== #
#   Global Plot Settings   #
# ======================== #

PLOT_PARAMS = {
    "tcpudp": [
        (
            "network_mean",
            TCPUDP_DELAY_STEPS,
            TCPUDP_DELAY_TICK_STEPS,
            map_value_to_color_index_ping_delay,
        ),
        (
            "network_p50",
            TCPUDP_DELAY_STEPS,
            TCPUDP_DELAY_TICK_STEPS,
            map_value_to_color_index_ping_delay,
        ),
        (
            "network_p99",
            TCPUDP_DELAY_STEPS,
            TCPUDP_DELAY_TICK_STEPS,
            map_value_to_color_index_ping_delay,
        ),
        (
            "failure_ratio",
            TCPUDP_RATIO_STEPS,
            TCPUDP_RATIO_TICK_STEPS,
            map_value_to_color_index_ratio,
        ),
        (
            "weird_ratio",
            TCPUDP_RATIO_STEPS,
            TCPUDP_RATIO_TICK_STEPS,
            map_value_to_color_index_ratio,
        ),
    ],
    "rdma": [
        (
            "network_mean",
            RDMA_DELAY_STEPS,
            RDMA_DELAY_TICK_STEPS,
            map_value_to_color_index_ping_delay,
        ),
        (
            "network_p50",
            RDMA_DELAY_STEPS,
            RDMA_DELAY_TICK_STEPS,
            map_value_to_color_index_ping_delay,
        ),
        (
            "network_p99",
            RDMA_DELAY_STEPS,
            RDMA_DELAY_TICK_STEPS,
            map_value_to_color_index_ping_delay,
        ),
        (
            "failure_ratio",
            RDMA_RATIO_STEPS,
            RDMA_RATIO_TICK_STEPS,
            map_value_to_color_index_ratio,
        ),
        (
            "weird_ratio",
            RDMA_RATIO_STEPS,
            RDMA_RATIO_TICK_STEPS,
            map_value_to_color_index_ratio,
        ),
    ],
}


# ================================================= #
#   Top-Level Functions to Generate Plots (Picklable)
# ================================================= #


def process_html_plot(
    records: list[dict],
    metric: str,
    time_name: str,
    steps: list,
    tick_steps: list[str],
    map_func,
    outname: str,
) -> str:
    """
    Top-level function that creates/saves the Plotly heatmap HTML.
    Returns the outname (no extension) on success, else an empty string.
    """
    result_path = plot_heatmap_value_to_html(
        records, metric, time_name, steps, tick_steps, map_func, outname
    )
    return outname if result_path else ""


def process_png_plot(
    records: list[dict],
    metric: str,
    time_name: str,
    steps: list,
    tick_steps: list[str],
    map_func,
    outname: str,
) -> str:
    """
    Top-level function that creates/saves the Matplotlib heatmap PNG.
    Returns the outname (no extension) on success, else an empty string.
    """
    result_path = plot_heatmap_value_to_png(
        records, metric, time_name, steps, tick_steps, map_func, outname
    )
    return outname if result_path else ""


# ======================== #
#      Plotting Helpers    #
# ======================== #


def plot_heatmap_value_to_html(
    records: list[dict],
    value_name: str,
    time_name: str,
    steps: list,
    tick_steps: list[str],
    map_func,
    outname: str,
) -> str:
    """
    Creates a heatmap Plotly figure from the records (list of dicts) and saves
    HTML files to the specified directories. Returns the path
    to the HTML file on success; returns an empty string on failure.
    """
    now_plot_time = time.time()
    try:
        if len(tick_steps) != len(COLOR_SCALE):
            msg = (
                f"Length of tick_steps must match length of COLOR_SCALE: "
                f"{len(tick_steps)} vs {len(COLOR_SCALE)}"
            )
            logger.error(msg)
            raise RuntimeError(msg)

        df = pd.DataFrame(records)

        source_ips = df["source"].unique()
        destination_ips = df["destination"].unique()

        source_ip_to_index = {ip: idx for idx, ip in enumerate(source_ips)}
        destination_ip_to_index = {ip: idx for idx, ip in enumerate(destination_ips)}
        df["source_idx"] = df["source"].map(source_ip_to_index)
        df["destination_idx"] = df["destination"].map(destination_ip_to_index)

        pivot_table = df.pivot(
            index="destination_idx", columns="source_idx", values=value_name
        ).fillna(-1)
        z_values = pivot_table.values

        time_pivot = df.pivot(
            index="destination_idx", columns="source_idx", values=time_name
        ).fillna("N/A")
        time_values = time_pivot.values

        # Prepare text for hover
        text_matrix = []
        for i in range(z_values.shape[0]):
            row_texts = []
            for j in range(z_values.shape[1]):
                src = source_ips[j]
                dst = destination_ips[i]
                val = z_values[i][j]
                if val >= 1:
                    formatted_val = f"{int(val):,}"
                elif 0 <= val < 1:
                    formatted_val = f"{val:.2f}"
                else:
                    formatted_val = "N/A"
                ping_time = time_values[i][j]
                text = f"Src: {src}<br>Dst: {dst}<br>Value: {formatted_val}<br>Time: {ping_time}"
                row_texts.append(text)
            text_matrix.append(row_texts)

        # Vectorize color mapping
        vectorized_map_func = np.vectorize(lambda x: map_func(x, steps))
        z_colors = vectorized_map_func(z_values)

        num_x = len(pivot_table.columns)
        num_y = len(pivot_table.index)

        # Dynamically calculate xgap and ygap
        xgap = max(0.2, int(20 / num_x))  # just a heuristic
        ygap = max(0.2, int(20 / num_y))  # just a heuristic

        fig = go.Figure(
            data=go.Heatmap(
                z=z_colors,
                colorscale=COLOR_SCALE,
                x=pivot_table.columns.values,
                y=pivot_table.index.values,
                zmin=0,
                zmax=len(tick_steps) - 1,
                xgap=xgap,
                ygap=ygap,
                customdata=text_matrix,
                hovertemplate="%{customdata}",
                hoverinfo="text",
                name="",
                colorbar=dict(
                    tickmode="array",
                    tickvals=list(range(len(tick_steps))),
                    ticktext=tick_steps,
                    title=value_name,
                ),
            )
        )

        current_time = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
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

        # Hide axis tick labels
        fig.update_xaxes(visible=True, showticklabels=False)
        fig.update_yaxes(visible=True, showticklabels=False)

        # Save to HTML
        html_path = os.path.join(HTML_DIR, f"{outname}.html")
        fig.write_html(html_path)

        elapsed_time = time.time() - now_plot_time
        logger.info(f"{outname}.html - elapsed time: {elapsed_time} seconds")

        return html_path

    except Exception as e:
        logger.error(f"plot_heatmap_value_to_html exception: {outname} - {e}")
        return ""


def plot_heatmap_value_to_png(
    records: list[dict],
    value_name: str,
    time_name: str,
    steps: list,
    tick_steps: list[str],
    map_func,
    outname: str,
) -> str:
    """
    Creates a heatmap from the records (list of dicts) and saves it as a PNG file.
    Returns the path to the PNG file on success; returns an empty string on failure.
    """
    now_plot_time = time.time()
    fig = None
    try:
        # Create a DataFrame from the input records
        df = pd.DataFrame(records)

        source_ips = df["source"].unique()
        destination_ips = df["destination"].unique()

        source_ip_to_index = {ip: idx for idx, ip in enumerate(source_ips)}
        destination_ip_to_index = {ip: idx for idx, ip in enumerate(destination_ips)}
        df["source_idx"] = df["source"].map(source_ip_to_index)
        df["destination_idx"] = df["destination"].map(destination_ip_to_index)

        # Create a pivot table for the heatmap values
        pivot_table = df.pivot(
            index="destination_idx", columns="source_idx", values=value_name
        ).fillna(-1)
        z_values = pivot_table.values

        # Vectorize color mapping for z-values
        vectorized_map_func = np.vectorize(lambda x: map_func(x, steps))
        z_colors = vectorized_map_func(z_values)

        # Use the COLOR_SCALE
        cmap = mcolors.ListedColormap(COLOR_SCALE)
        norm = mcolors.BoundaryNorm(range(len(tick_steps) + 1), cmap.N)

        # Plot the heatmap
        fig, ax = plt.subplots(figsize=(10, 8))
        cax = ax.imshow(z_colors, cmap=cmap, norm=norm, aspect="auto")

        # Add colorbar
        cbar = fig.colorbar(cax, ax=ax, ticks=[x + 0.5 for x in range(len(tick_steps))])
        cbar.ax.set_yticklabels(tick_steps)
        cbar.set_label(value_name, rotation=270, labelpad=15)

        # Hide axis tick labels
        ax.set_xticks([])
        ax.set_yticks([])

        # Add title
        current_time = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
        ax.set_title(f"{outname} ({current_time})")

        # Force layout before saving
        fig.canvas.draw()

        # Save the figure to PNG
        png_path = os.path.join(IMAGE_DIR, f"{outname}.png")
        plt.savefig(png_path, bbox_inches="tight", dpi=100)
        plt.close(fig)

        elapsed_time = time.time() - now_plot_time
        logger.info(f"{outname}.png - elapsed time: {elapsed_time} seconds")
        return png_path

    except Exception as e:
        logger.error(f"plot_heatmap_value_to_png exception: {outname} - {e}")
        if fig:
            plt.close(fig)
        return ""


# ======================== #
#     Data Calculations    #
# ======================== #


def calculate_ratios(
    n_success: int, n_failure: int, n_weird: int
) -> tuple[float, float]:
    """
    Given success, failure, and weird counts, returns a tuple of (failure_ratio, weird_ratio).
    If total is zero, returns -1 for the ratio.
    """
    total_sf = n_success + n_failure
    total_all = n_success + n_failure + n_weird

    failure_ratio = -1 if total_sf == 0 else n_failure / total_sf
    weird_ratio = -1 if total_all == 0 else n_weird / total_all

    return failure_ratio, weird_ratio


def prepare_default_record(src: str, dst: str, plot_type: str) -> dict:
    """
    Prepares a default record dictionary for a given source, destination, and plot_type.
    """
    default_record = {
        "source": src,
        "destination": dst,
        "failure_ratio": -1,
        "weird_ratio": -1,
        "network_mean": -1,
        "network_p50": -1,
        "network_p99": -1,
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

    return default_record


def prepare_record(key: str, value: list[str], plot_type: str) -> dict:
    """
    Given a key in the format 'src,dst' and a corresponding Redis value (list of strings),
    returns a dictionary suitable for plotting. If value is None or empty, returns
    a default record.
    """
    src, dst = key.split(",")
    if not value:
        return prepare_default_record(src, dst, plot_type)

    ts_ping_start, ts_ping_end, n_success, n_failure, n_weird = value[:5]
    n_success = int(n_success)
    n_failure = int(n_failure)
    n_weird = int(n_weird)
    failure_ratio, weird_ratio = calculate_ratios(n_success, n_failure, n_weird)

    if plot_type == "tcpudp":
        # indexes: [5=_, 6=network_mean, 7=_, 8=network_p50, 9=_, 10=network_p99]
        _, network_mean, _, network_p50, _, network_p99 = value[5:11]
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

    # RDMA
    # indexes:
    #   [5=_, 6=client_mean, 7=_, 8=client_p50, 9=_, 10=client_p99,
    #    11=_, 12=network_mean, 13=_, 14=network_p50, 15=_, 16=network_p99,
    #    17=_, 18=server_mean, 19=_, 20=server_p50, 21=_, 22=server_p99]
    _, client_mean, _, client_p50, _, client_p99 = value[5:11]
    _, network_mean, _, network_p50, _, network_p99 = value[11:17]
    _, server_mean, _, server_p50, _, server_p99 = value[17:23]

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


# ======================== #
#   Plot Heatmap (MP)      #
# ======================== #


def plot_heatmap(
    data: dict[str, list[str]],
    category_group_name: str = "group",
    plot_type: str = "tcpudp",
) -> list[str]:
    """
    Creates multiple heatmaps based on the given plot_type (tcpudp or rdma).
    Returns a list of output file names (NO extension).
    """
    records = [prepare_record(k, v, plot_type) for k, v in data.items()]
    output_files = []
    params_for_type = PLOT_PARAMS.get(plot_type, [])

    # Use ProcessPoolExecutor for parallel plotting
    # (Creating a fresh executor inside this function ensures children
    #  won't outlive their immediate usage if we exit mid-way.)
    with ProcessPoolExecutor() as executor:
        # 1) Plotly (HTML) tasks
        futures_html = []
        for metric, steps, tick_steps, color_index in params_for_type:
            outname = f"{category_group_name}_{metric}"
            fut = executor.submit(
                process_html_plot,
                records,  # picklable data only
                metric,
                "ping_end_time",  # or whichever field you want for time
                steps,
                tick_steps,
                color_index,
                outname,
            )
            futures_html.append(fut)

        # 2) Matplotlib (PNG) tasks
        futures_png = []
        for metric, steps, tick_steps, color_index in params_for_type:
            outname = f"{category_group_name}_{metric}"
            fut = executor.submit(
                process_png_plot,
                records,
                metric,
                "ping_end_time",
                steps,
                tick_steps,
                color_index,
                outname,
            )
            futures_png.append(fut)

        # Gather HTML results
        for f in futures_html:
            result = f.result()  # returns outname or ""
            if result:
                output_files.append(result)

        # Gather PNG results
        for f in futures_png:
            result = f.result()
            if result:
                output_files.append(result)

    return output_files


def process_category_group(
    category: str, group: str, group_data: dict[str, list[str]]
) -> list[str]:
    """
    Process a single category and group combination, generating the relevant heatmap.
    Returns a list of the resulting plot files created (without extension).
    """
    if category in {"udp", "tcp"}:
        return plot_heatmap(group_data, f"{category}_{group}", "tcpudp")
    elif category in {"roce", "ib"}:
        return plot_heatmap(group_data, f"{category}_{group}", "rdma")
    return []


# ======================== #
#    Main Async Function   #
# ======================== #

STOP_EVENT = asyncio.Event()


async def pingweave_plotter() -> None:
    """
    Main asynchronous plotting task. It periodically fetches data from Redis,
    generates heatmap plots, and cleans up old files.
    """
    load_config_ini()
    last_plot_time = int(time.time())
    pinglist_protocol = ["udp", "tcp", "roce", "ib"]

    try:
        while not STOP_EVENT.is_set():
            now_plot_time = int(time.time())
            interval_seconds = 1
            if interval_report_ping_result_millisec:
                interval_seconds = int(interval_report_ping_result_millisec / 1000)

            # Plot the graph every X seconds
            if (last_plot_time + interval_seconds < now_plot_time) and (
                redis_server is not None
            ):
                last_plot_time = now_plot_time

                # Step 1: Load pinglist into memory
                read_pinglist()

                # Step 2: Prepare template records and IP->group mapping
                records = copy.deepcopy(pinglist_in_memory)
                map_ip_to_groups = {}

                for proto, cat_data in pinglist_in_memory.items():
                    for group, ip_list in cat_data.items():
                        # Make a template for each possible src-dst combination
                        records[proto][group] = {
                            f"{src},{dst}": None for src in ip_list for dst in ip_list
                        }

                        # Build IP -> groups mapping
                        for ip in ip_list:
                            if ip not in map_ip_to_groups:
                                map_ip_to_groups[ip] = set()
                            map_ip_to_groups[ip].add(group)

                # Step 3: Read data from Redis
                cursor = "0"
                while cursor != 0:  # IMPORTANT: != 0 (do not use != "0")
                    current_time = datetime.now()
                    cursor, keys = redis_server.scan(cursor=cursor)
                    for key in keys:
                        value = redis_server.get(key)
                        if value is None:
                            logger.warning(f"Redis key {key} not found. Skipping.")
                            continue

                        value_splits = value.split(",")
                        # Filter out old data
                        try:
                            measure_time = datetime.strptime(
                                value_splits[1][:26], "%Y-%m-%d %H:%M:%S.%f"
                            )
                        except ValueError:
                            logger.debug(
                                f"Invalid datetime format in key {key}. Skipping."
                            )
                            continue

                        time_diff = abs((current_time - measure_time).total_seconds())
                        if time_diff > INTERVAL_PLOTTER_FILTER_OLD_DATA_SEC:
                            logger.debug(
                                f"Ignore old data: {key} | Time: {measure_time} | T_Diff: {time_diff}"
                            )
                            continue

                        proto, src, dst = key.split(",")
                        if proto not in pinglist_protocol:
                            raise ValueError(f"Unexpected protocol type: {proto}")

                        record_key = f"{src},{dst}"
                        src_groups = map_ip_to_groups.get(src, set())
                        dst_groups = map_ip_to_groups.get(dst, set())
                        common_groups = list(src_groups & dst_groups)

                        # Insert valid data into our records structure
                        for group in common_groups:
                            if group in records[proto]:
                                if record_key not in records[proto][group]:
                                    raise KeyError(
                                        f"{record_key} is not in records[{proto}][{group}]"
                                    )
                                records[proto][group][record_key] = value_splits

                # Step 4: Generate plots (concurrently for each category and group)
                new_file_list = []
                # We'll do a top-level executor usage here for concurrency
                with ProcessPoolExecutor() as executor:
                    futures = []
                    for category, data in records.items():
                        for group, group_data in data.items():
                            futures.append(
                                executor.submit(
                                    process_category_group,
                                    category,
                                    group,
                                    group_data,
                                )
                            )

                    for future in futures:
                        new_file_list.extend(future.result())

                # Step 5: Cleanup stale HTML/PNG files
                clear_directory_conditional(HTML_DIR, new_file_list, "html")
                clear_directory_conditional(IMAGE_DIR, new_file_list, "png")

                elapsed_time = time.time() - now_plot_time
                logger.info(
                    f"Pingweave plotter is running on {control_host}:{collect_port} | "
                    f"Total elapsed time: {elapsed_time:.2f} seconds"
                )

            await asyncio.sleep(1)

    except Exception as e:
        logger.error(f"Exception in pingweave_plotter: {e}")
    finally:
        logger.info("pingweave_plotter task is shutting down.")


def handle_signal(signum, frame):
    """
    Signal handler to ensure a graceful shutdown when SIGINT or SIGTERM is received.
    """
    logger.info(f"Received signal {signum}. Stopping plotter...")
    STOP_EVENT.set()


def run_pingweave_plotter() -> None:
    """
    Entry point for the plotter script. Sets process title, installs signal handlers,
    runs the async plotter, and handles cleanup gracefully.
    """
    # Give the process a recognizable title
    # setproctitle("pingweave_plotter.py")

    # Register signal handlers for normal kill signals
    signal.signal(signal.SIGINT, handle_signal)
    signal.signal(signal.SIGTERM, handle_signal)

    try:
        asyncio.run(pingweave_plotter())
    except KeyboardInterrupt:
        logger.info("KeyboardInterrupt received in main. Exiting.")
    except Exception as e:
        logger.error(f"Exception in run_pingweave_plotter: {e}")
    finally:
        logger.info("Plotter main function is exiting now.")


if __name__ == "__main__":
    run_pingweave_plotter()
