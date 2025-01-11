import os
import sys
import signal
import configparser
import asyncio
import time
import socket
import psutil
import multiprocessing
import copy
from setproctitle import setproctitle
import re

from logger import initialize_pingweave_logger
import yaml  # python3 -m pip install pyyaml
from aiohttp import web  # requires python >= 3.7
from macro import *

logger = initialize_pingweave_logger(socket.gethostname(), "webserver", 10, False)

# Variables to save pinglist
pinglist_in_memory = {}
address_store = {}  # (for RDMA) ip -> (ip, gid, lid, qpn, dtime)
address_store_checkpoint = 0
pinglist_lock = asyncio.Lock()
address_store_lock = asyncio.Lock()

# Global variables
control_host = None
control_port = None
interval_sync_pinglist_sec = None
interval_read_pinglist_sec = None


# ConfigParser object
config = configparser.ConfigParser()


def check_ip_active(target_ip):
    """
    Checks if the given IP address is active and associated with an interface that is UP.
    """
    try:
        # Get network interface info from psutil
        net_if_addrs = psutil.net_if_addrs()
        net_if_stats = psutil.net_if_stats()

        for iface, addrs in net_if_addrs.items():
            for addr in addrs:
                if addr.family == socket.AF_INET and addr.address == target_ip:
                    # Check interface status
                    if iface in net_if_stats and net_if_stats[iface].isup:
                        return True
                    else:
                        logger.error(f"Interface {iface} with IP {target_ip} is down.")
                        return False

        # No match
        logger.error(f"No active interface found with IP address {target_ip}.")
        return False

    except Exception as e:
        logger.error(f"An unexpected error occurred while checking IP: {e}")
        return False


def load_config_ini():
    """
    Reads the configuration file and updates global variables.
    """
    global control_host, control_port, interval_sync_pinglist_sec, interval_read_pinglist_sec

    try:
        config.read(CONFIG_PATH)

        # Update variables
        control_host = config["controller"]["host"]
        control_port = int(config["controller"]["port_control"])

        interval_sync_pinglist_sec = int(config["param"]["interval_sync_pinglist_sec"])
        interval_read_pinglist_sec = int(config["param"]["interval_read_pinglist_sec"])

        logger.debug(f"Configuration reloaded successfully from {CONFIG_PATH}.")

    except Exception as e:
        logger.error(f"Error reading configuration: {e}")
        logger.error(
            "Using default parameters: interval_sync_pinglist_sec=60, interval_read_pinglist_sec=60"
        )
        interval_sync_pinglist_sec = 60
        interval_read_pinglist_sec = 60


async def read_pinglist():
    global pinglist_in_memory

    try:
        tmp_pinglist_in_memory = None
        if os.path.isfile(PINGLIST_PATH):
            with open(PINGLIST_PATH, "r") as file:
                tmp_pinglist_in_memory = yaml.safe_load(file)
                logger.debug(f"{PINGLIST_PATH} yaml was loaded successfully.")

            async with pinglist_lock:
                pinglist_in_memory.clear()
                pinglist_in_memory = copy.deepcopy(tmp_pinglist_in_memory)

        else:
            logger.error(f"Pinglist file not found at {PINGLIST_PATH}")
    except Exception as e:
        logger.error(f"Error loading pinglist: {e}")


async def read_pinglist_periodically():
    load_config_ini()
    try:
        while True:
            await read_pinglist()
            await asyncio.sleep(interval_read_pinglist_sec)
    except asyncio.CancelledError:
        logger.info("read_pinglist_periodically task was cancelled.")
    except Exception as e:
        logger.error(f"Exception in read_pinglist_periodically: {e}")


async def get_pinglist(request):
    client_ip = request.remote
    async with pinglist_lock:
        response_data = pinglist_in_memory
    logger.debug(f"(SEND) pinglist.yaml to client: {client_ip}")
    return web.json_response(response_data)


async def get_address_store(request):
    current_time = int(time.time())
    client_ip = request.remote
    async with address_store_lock:
        global address_store_checkpoint

        # condition to filter old entries (every 1 minute)
        if address_store_checkpoint + 60 < current_time:
            # get keys which was not updated in last 5 minutes
            keys_old_entries = [
                key
                for key, value in address_store.items()
                if value[5] + 300 < current_time
            ]
            for key in keys_old_entries:
                logger.info(f"(EXPIRED) Remove old address information: {key}")
                address_store.pop(key)
            address_store_checkpoint = current_time
        response_data = address_store
    logger.debug(f"(SEND) address_store to client: {client_ip}")
    return web.json_response(response_data)


async def post_address(request):
    client_ip = request.remote
    try:
        data = await request.json()
        ip_address = data.get("ip_address")
        gid = data.get("gid")
        lid = data.get("lid")
        qpn = data.get("qpn")
        dtime = data.get("dtime")
        utime = int(time.time())

        if all([ip_address, gid, lid, qpn, dtime]):
            async with address_store_lock:
                address_store[ip_address] = [
                    ip_address,
                    gid,
                    int(lid),
                    int(qpn),
                    str(dtime),
                    int(utime),
                ]
                logger.debug(
                    f"(RECV) POST from {client_ip}. Updated address store (size: {len(address_store)})."
                )

                if len(address_store) > 10000:
                    logger.critical(
                        f"Too many entries in address_store: {len(address_store)}"
                    )
                    logger.error("Cleaning up address_store. Check your configuration.")
                    address_store.clear()
            return web.Response(text="Address updated", status=200)
        else:
            logger.warning(f"(RECV) Incorrect POST format from {client_ip}")
            return web.Response(text="Invalid data", status=400)
    except Exception as e:
        logger.error(f"Error processing POST from {client_ip}: {e}")
        return web.Response(text="Internal webserver error", status=500)


## simple index example
# async def index(request):
#     # 디렉토리 내의 파일 목록을 가져옵니다.
#     files = os.listdir(HTML_DIR)
#     # HTML 파일만 필터링합니다.
#     html_files = [f for f in files if f.endswith(".html")]
#     # 인덱스 페이지를 생성합니다.
#     file_links = [f'<li><a href="/{fname}">{fname}</a></li>' for fname in html_files]
#     content = f"""
#     <html>
#         <head><title>Available HTML Files</title></head>
#         <body>
#             <h1>Available PingWeave list</h1>
#             <ul>
#                 {''.join(file_links)}
#             </ul>
#         </body>
#     </html>
#     """
#     return web.Response(text=content, content_type="text/html")


async def index(request):
    files = os.listdir(HTML_DIR)
    html_files = [f for f in files if f.endswith(".html")]

    # 1) Group the HTML files by protocol -> group -> (fname, measure)
    grouped_files = {}
    for fname in html_files:
        name_only = fname.replace(".html", "")
        tokens = name_only.split("_")

        if len(tokens) < 3:
            protocol = "others"
            group_name = "others"
            measure = name_only
        else:
            protocol = tokens[0]
            measure = "_".join(tokens[-2:])
            middle_tokens = tokens[1:-2]
            group_name = "_".join(middle_tokens) if middle_tokens else "no_group"

        if protocol not in grouped_files:
            grouped_files[protocol] = {}
        if group_name not in grouped_files[protocol]:
            grouped_files[protocol][group_name] = []
        grouped_files[protocol][group_name].append((fname, measure))

    protocol_list = sorted(grouped_files.keys())

    # -------------------- Build HTML --------------------
    content = f"""
    <html>
      <head>
        <title>PingWeave Dashboard</title>
        <!-- Local Bootstrap CSS -->
        <link rel="stylesheet" href="/bootstrap/css/bootstrap.min.css">
        <!-- Local Bootstrap JS (including Popper) -->
        <script src="/bootstrap/js/bootstrap.bundle.min.js"></script>

        <script>
          // (A) Format date to "YYYY-MM-DD HH:mm:SS"
          function formatDateTime(dateObj) {{
            let year = dateObj.getFullYear();
            let month = String(dateObj.getMonth() + 1).padStart(2, '0');
            let day = String(dateObj.getDate()).padStart(2, '0');
            let hour = String(dateObj.getHours()).padStart(2, '0');
            let minute = String(dateObj.getMinutes()).padStart(2, '0');
            let second = String(dateObj.getSeconds()).padStart(2, '0');
            return year + "-" + month + "-" + day + " " + hour + ":" + minute + ":" + second;
          }}

          // (B) Called when an image finishes loading
          function recordLoad(imgElem) {{
            let now = Date.now();
            imgElem.dataset.loadedAt = now;
          }}

          // (C) Update each image's load time every second
          function updateImageTimes() {{
            let now = Date.now();
            let images = document.querySelectorAll('img[data-loaded-at]');
            images.forEach(img => {{
              let loadedTime = parseInt(img.dataset.loadedAt);
              if (!loadedTime) return;
              let diffSec = Math.floor((now - loadedTime) / 1000);
              let displayElem = img.parentNode.querySelector('.timeSinceImageLoad');
              if (displayElem) {{
                displayElem.innerText = diffSec + " seconds since this image loaded.";
              }}
            }});
          }}
          setInterval(updateImageTimes, 1000);

          // (D) Automatically refresh images every 10 seconds
          function reloadAllImages() {{
            let images = document.querySelectorAll('img[data-original-src]');
            images.forEach(img => {{
              let original = img.dataset.originalSrc;
              // Cache-busting query parameter
              let newSrc = original + "?_=" + Date.now();
              img.src = newSrc;
            }});
          }}
          setInterval(reloadAllImages, 10000);

          // (E) Show current time in "YYYY-MM-DD HH:MM:SS" at top-right
          function updateCurrentTime() {{
            let now = new Date();
            let timeString = formatDateTime(now); // e.g. 2025-11-21 21:13:26
            let currentTimeElem = document.getElementById('currentTime');
            if (currentTimeElem) {{
              currentTimeElem.textContent = timeString;
            }}
          }}
          setInterval(updateCurrentTime, 1000);

        </script>
      </head>
      <body class="bg-light">
        <!-- Container -->
        <div class="container my-4">
          <!-- (F) Current time at top-right -->
          <div id="currentTime" 
               class="text-secondary mb-2" 
               style="text-align:right; font-size:0.9em;">
          </div>

          <h1 class="mb-3">PingWeave Dashboard </h1>

          <p class="text-secondary">
            This PingWeave dashboard shows a grouped end-to-end latency monitoring with a mesh grid.<br>
          </p>

          <!-- Protocol Tabs -->
          <ul class="nav nav-tabs" id="protocolTab" role="tablist">
    """

    # Build protocol tabs
    first_protocol = True
    for protocol in protocol_list:
        active_class = "active" if first_protocol else ""
        aria_selected = "true" if first_protocol else "false"
        content += f"""
            <li class="nav-item" role="presentation">
              <button
                class="nav-link {active_class}"
                id="tab-{protocol}"
                data-bs-toggle="tab"
                data-bs-target="#panel-{protocol}"
                type="button"
                role="tab"
                aria-controls="panel-{protocol}"
                aria-selected="{aria_selected}">
                {protocol}
              </button>
            </li>
        """
        first_protocol = False

    content += """
          </ul>
          <div class="tab-content" id="protocolTabContent">
    """

    # Protocol tab panes
    first_protocol = True
    for protocol in protocol_list:
        show_active = "show active" if first_protocol else ""
        first_protocol = False

        content += f"""
            <div
              class="tab-pane fade {show_active}"
              id="panel-{protocol}"
              role="tabpanel"
              aria-labelledby="tab-{protocol}">

              <div class="container mt-4">
                <ul class="nav nav-pills mb-3" id="groupTab-{protocol}" role="tablist">
        """

        group_dict = grouped_files[protocol]
        group_list = sorted(group_dict.keys())

        first_group = True
        for group_name in group_list:
            active_class = "active" if first_group else ""
            aria_selected = "true" if first_group else "false"
            safe_group_id = f"{protocol}-{group_name}".replace(" ", "_")
            content += f"""
                  <li class="nav-item" role="presentation">
                    <button
                      class="nav-link {active_class}"
                      id="tab-{safe_group_id}"
                      data-bs-toggle="pill"
                      data-bs-target="#panel-{safe_group_id}"
                      type="button"
                      role="tab"
                      aria-controls="panel-{safe_group_id}"
                      aria-selected="{aria_selected}">
                      {group_name}
                    </button>
                  </li>
            """
            first_group = False

        content += f"""
                </ul>
                <div class="tab-content" id="groupTabContent-{protocol}">
        """

        first_group = True
        for group_name in group_list:
            show_active = "show active" if first_group else ""
            first_group = False
            safe_group_id = f"{protocol}-{group_name}".replace(" ", "_")

            file_list = group_dict[group_name]
            file_list = sorted(file_list, key=lambda x: x[1])

            content += f"""
                  <div
                    class="tab-pane fade {show_active}"
                    id="panel-{safe_group_id}"
                    role="tabpanel"
                    aria-labelledby="tab-{safe_group_id}">

                    <h5 class="mb-3">Group: {group_name} ({len(file_list)} files)</h5>
                    <ul class="list-unstyled">
            """
            for fname, measure in file_list:
                base_name = fname.replace(".html", "")
                original_src = f"/image/{base_name}.png"
                content += f"""
                      <li class="mb-4 border-bottom pb-3">
                        <div><strong>Measure:</strong> {measure}</div>
                        <div><strong>Filename:</strong> <a href="/{fname}" target="_blank">{fname}</a></div>
                        <div class="mt-2">
                          <img src="{original_src}"
                               data-original-src="{original_src}"
                               alt="{base_name}"
                               onload="recordLoad(this)"
                               data-loaded-at=""
                               style="max-width:600px; border:1px solid #aaa; border-radius:3px;">
                          <div class="timeSinceImageLoad text-muted small mt-1"></div>
                        </div>
                      </li>
                """

            content += """
                    </ul>
                  </div>
            """

        content += """
                </div> <!-- end groupTabContent -->
              </div> <!-- end container -->
            </div> <!-- end protocol tab pane -->
        """

    content += """
          </div> <!-- end protocolTabContent -->
        </div> <!-- end container -->

        <script>
          // Trigger the first time update for currentTime
          updateCurrentTime();
        </script>
      </body>
    </html>
    """

    return web.Response(text=content, content_type="text/html")


async def pingweave_webserver():
    load_config_ini()

    try:
        while True:
            if not check_ip_active(control_host):
                logger.error(
                    f"No active interface with Control IP {control_host}. Sleeping for 1 minute..."
                )
                await asyncio.sleep(60)
                load_config_ini()
                continue

            try:
                app = web.Application()
                app.router.add_get("/", index)  # indexing for html files
                app.router.add_static("/", HTML_DIR)  # static route for html files
                app.router.add_static("/image", IMAGE_DIR)  # static route for images
                app.router.add_static("/bootstrap", BOOTSTRAP_DIR)  # for bootstrap
                app.router.add_get("/pinglist", get_pinglist)
                app.router.add_get("/address_store", get_address_store)
                app.router.add_post("/address", post_address)

                runner = web.AppRunner(app)
                await runner.setup()
                site = web.TCPSite(runner, host="0.0.0.0", port=control_port)
                await site.start()

                logger.info(
                    f"Pingweave webserver running on {control_host}:{control_port}"
                )

                asyncio.create_task(read_pinglist_periodically())

                await asyncio.Event().wait()

            except asyncio.CancelledError:
                logger.info("pingweave_webserver task was cancelled.")
                break
            except Exception as e:
                logger.error(f"Cannot start the pingweave webserver: {e}")
            finally:
                await runner.cleanup()
                await asyncio.sleep(10)
    except KeyboardInterrupt:
        logger.info("pingweave_webserver received KeyboardInterrupt. Exiting.")
    except Exception as e:
        logger.error(f"Exception in pingweave_webserver: {e}")


def run_pingweave_webserver():
    setproctitle("pingweave_webserver.py")
    try:
        asyncio.run(pingweave_webserver())
    except KeyboardInterrupt:
        logger.info("pingweave_webserver process received KeyboardInterrupt. Exiting.")
