import redis  # in-memory key-value storage
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

logger = initialize_pingweave_logger(socket.gethostname(), "plotter")

# Global variables
control_host = None
collect_port = None

# Variables to save pinglist
pinglist_in_memory = {}

# ConfigParser object
config = configparser.ConfigParser()

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
        print(f"Socket file does not exist: {socket_path}")
    redis_server = None
except FileNotFoundError as e:
    logger.error(f"Redis socket file does not exist: {e}")
    redis_server = None
except Exception as e:
    logger.error(f"Unexpected error of Redis server: {e}")
    redis_server = None


def load_config_ini():
    global control_host, collect_port

    try:
        config.read(CONFIG_PATH)
        control_host = config["controller"]["host"]
        collect_port = int(config["controller"]["port_collect"])
        logger.debug("Configuration loaded successfully from config file.")
    except Exception as e:
        logger.error(f"Error reading configuration: {e}")
        control_host = "0.0.0.0"
        collect_port = 8080


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


def plot_heatmap(records, outname="result"):
    # records = [
    #     {"source": src, "destination": dst, "value": val, "time": time}
    #     for (src, dst), (val, time) in data.items()
    # ]

    # 데이터를 리스트로 변환하여 DataFrame 생성
    df = pd.DataFrame(records)

    # IP 주소를 인덱스로 매핑
    source_ips = df["source"].unique()
    destination_ips = df["destination"].unique()
    source_ip_to_index = {ip: idx for idx, ip in enumerate(source_ips)}
    destination_ip_to_index = {ip: idx for idx, ip in enumerate(destination_ips)}
    df["source_idx"] = df["source"].map(source_ip_to_index)
    df["destination_idx"] = df["destination"].map(destination_ip_to_index)

    # 피벗 테이블 생성 (value)
    pivot_table = df.pivot(
        index="destination_idx", columns="source_idx", values="value"
    ).fillna(0)
    z_values = pivot_table.values

    # 인덱스를 IP 주소로 매핑
    index_to_source_ip = {idx: ip for ip, idx in source_ip_to_index.items()}
    index_to_destination_ip = {idx: ip for ip, idx in destination_ip_to_index.items()}

    # 호버 데이터 생성
    x_indices = pivot_table.columns.values
    y_indices = pivot_table.index.values
    x_ips = [index_to_source_ip[idx] for idx in x_indices]
    y_ips = [index_to_destination_ip[idx] for idx in y_indices]
    x_mesh, y_mesh = np.meshgrid(x_ips, y_ips)

    # 'time' 데이터를 피벗 테이블 형태로 변환
    time_pivot = df.pivot(index="destination_idx", columns="source_idx", values="time")
    time_values = time_pivot.values
    time_values = np.where(pd.isnull(time_values), "N/A", time_values)

    # customdata 생성
    customdata = np.dstack((x_mesh, y_mesh, time_values))

    # 히트맵 생성
    fig = go.Figure(
        data=go.Heatmap(
            z=z_values,
            colorscale="Viridis",
            colorbar=dict(title="Value"),
            hoverongaps=False,
            customdata=customdata,
            x=x_indices,
            y=y_indices,
        )
    )

    # 호버 템플릿 설정
    fig.update_traces(
        hovertemplate="Source IP: %{customdata[0]}<br>"
        + "Destination IP: %{customdata[1]}<br>"
        + "Value: %{z}<br>"
        + "Time: %{customdata[2]}<extra></extra>"
    )

    # 레이아웃 업데이트
    fig.update_layout(
        xaxis_title="Source IP",
        yaxis_title="Destination IP",
        title="Source-Destination Heatmap",
    )

    # 축 라벨 숨기기
    fig.update_xaxes(visible=False)
    fig.update_yaxes(visible=False)

    # HTML 파일로 저장
    fig.write_html(f"{HTML_DIR}/{outname}.html")


async def pingweave_plotter():
    load_config_ini()
    last_plot_time = int(time.time())
    pinglist_protocol = ["tcp", "rdma"]

    try:
        while True:
            if not check_ip_active(control_host):
                logger.info(
                    f"No active interface with Control IP {control_host}. Sleep 1 minute..."
                )
                await asyncio.sleep(60)
                load_config_ini()
                continue

            try:
                # plot the graph for every 30 seconds
                now = int(time.time())
                if last_plot_time + 10 < now and redis_server != None:
                    # update the last plot time
                    last_plot_time = now

                    logger.info(
                        f"Pingweave plotter is running on {control_host}:{collect_port}"
                    )

                    # read pinglist (synchronous)
                    read_pinglist()

                    # create template records
                    # pinglist = {'tcp': {'group1': ['192.168.1.1', '192.168.1.2', '192.168.1.3']}}
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

                    # insert process
                    cursor = "0"
                    while cursor != 0:
                        cursor, keys = redis_server.scan(cursor=cursor)  # scan kv-store
                        for key in keys:
                            # TODO: skip to insert if no key
                            value = redis_server.get(key)
                            proto, src, dst = key.split(",")
                            record_key = f"{src},{dst}"
                            if proto not in pinglist_protocol:
                                raise Exception(f"Not expected protocol type: {proto}")

                            # check which groups to insert
                            # # TODO: if no key, error handling
                            src_groups = map_ip_to_groups[src]
                            dst_groups = map_ip_to_groups[dst]
                            common_groups = list(set(src_groups) & set(dst_groups))

                            # insertion
                            for group in common_groups:
                                if record_key not in records[proto][group]:
                                    raise Exception(
                                        f"{record_key} is not in records[{proto}][{group}]"
                                    )
                                records[proto][group][record_key] = value.split(",")

                # category/group 별로 plot 그리기

                records

            except Exception as e:
                logger.error(f"Cannot run the pingweave plotter: {e}")
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
