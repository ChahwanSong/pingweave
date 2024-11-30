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
        logger.error(f"Socket file does not exist: {socket_path}")
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


def plot_heatmap_value(records, value_name, time_name, outname):
    print(f"Plotting a heatmap of {outname}")
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
        index="destination_idx", columns="source_idx", values=value_name
    ).fillna(-1)
    z_values = pivot_table.values

    # 시간 데이터 피벗 테이블 생성
    time_pivot = df.pivot(
        index="destination_idx", columns="source_idx", values=time_name
    ).fillna("N/A")
    time_values = time_pivot.values

    # 텍스트 매트릭스 생성
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

    # 값에 따라 색상 인덱스로 매핑하는 함수 정의
    def map_value_to_color_index(value):
        if value == -1:
            return 0  # 검은색
        elif 0 <= value < 100000:
            return 1  # 초록색
        elif 100000 <= value < 500000:
            return 2  # 노란색
        elif 500000 <= value < 5000000:
            return 3  # 주황색
        elif value >= 5000000:
            return 4  # 빨간색
        else:
            return 5  # 기타 경우는 보라색

    # 함수 벡터화 및 적용
    map_func = np.vectorize(map_value_to_color_index)
    z_colors = map_func(z_values)

    # 색상 스케일 정의
    colorscale = ["black", "green", "yellow", "orange", "red", "purple"]

    # 셀의 수 계산
    num_x = len(pivot_table.columns)
    num_y = len(pivot_table.index)

    # xgap과 ygap을 셀의 수에 따라 동적으로 설정
    xgap = max(1, int(20 / num_x))
    ygap = max(1, int(20 / num_y))

    # 히트맵 생성
    fig = go.Figure(
        data=go.Heatmap(
            z=z_colors,
            colorscale=colorscale,
            x=pivot_table.columns.values,
            y=pivot_table.index.values,
            zmin=0,  # 최소값 설정
            zmax=5,  # 최대값 설정
            xgap=xgap,  # 동적으로 계산된 수평 여백
            ygap=ygap,  # 동적으로 계산된 수직 여백
            customdata=text_matrix,  # customdata에 텍스트 매트릭스 전달
            hovertemplate="%{customdata}",  # 마우스 오버 시 텍스트 표시
            hoverinfo="text",  # 호버 시 텍스트 정보 사용
            name="",  # trace 이름을 빈 문자열로 설정하여 'trace 0' 제거
            colorbar=dict(
                tickmode="array",
                tickvals=[0, 1, 2, 3, 4, 5],
                ticktext=["No Data", "~100µs", "~500µs", "~5ms", ">5ms", "Unknown"],
                title=value_name,
            ),
        )
    )

    # 레이아웃 업데이트 (배경색 설정)
    fig.update_layout(
        xaxis_title="Source IP",
        yaxis_title="Destination IP",
        title=f"{outname}",
        plot_bgcolor="white",
        paper_bgcolor="white",
    )

    # 축 라벨 숨기기 (필요에 따라 표시 가능)
    fig.update_xaxes(visible=False)
    fig.update_yaxes(visible=False)

    # HTML 파일로 저장
    fig.write_html(f"{HTML_DIR}/{outname}.html")


def plot_heatmap(data, outname="result"):
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
                    "network_p99": -1,
                    "client_p99": -1,
                    "server_p99": -1,
                    "ping_start_time": "N/A",
                    "ping_end_time": "N/A",
                }
            )

    plot_heatmap_value(
        records, "network_mean", "ping_end_time", outname + "_network_mean"
    )


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
                    current_time = datetime.now()
                    while cursor != 0:
                        cursor, keys = redis_server.scan(cursor=cursor)  # scan kv-store
                        for key in keys:
                            value = redis_server.get(key)
                            if value is None:
                                logger.warning(
                                    f"Redis Key {key} not found in Redis. Skipping..."
                                )
                                continue

                            value = value.split(",")

                            # filtering old information
                            measure_time = datetime.strptime(
                                value[1][:26], "%Y-%m-%d %H:%M:%S.%f"
                            )
                            # calculate a time difference
                            time_difference = abs(
                                (current_time - measure_time).total_seconds()
                            )

                            # skip if the info is stale more than 30 seconds
                            if time_difference > 30:
                                continue

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
                                records[proto][group][record_key] = value

                    # category/group 별로 plot 그리기
                    for category, data in records.items():
                        for group, group_data in data.items():
                            plot_heatmap(group_data, f"{category}_{group}")

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
