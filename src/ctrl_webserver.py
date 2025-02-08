#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import asyncio
import html
import psutil
from datetime import datetime
from setproctitle import setproctitle

import yaml  # python3 -m pip install pyyaml
from aiohttp import web  # requires python >= 3.7
from macro import *

from logger import initialize_pingweave_logger
from common import *

logger = initialize_pingweave_logger(socket.gethostname(), "ctrl_webserver", 10, False)

# Variables to save pinglist
pinglist_in_memory = {}
address_store = {}  # (for RDMA) ip -> (ip, gid, lid, qpn, dtime)
address_store_checkpoint = 0
pinglist_lock = asyncio.Lock()
address_store_lock = asyncio.Lock()


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
    try:
        while True:
            await read_pinglist()
            await asyncio.sleep(config["interval_read_pinglist_sec"])
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
        global address_store, address_store_checkpoint

        # condition to filter old entries (every 1 minute)
        if address_store_checkpoint + 60 < current_time:
            # get keys which was not updated in last 5 minutes
            keys_old_entries = [
                key
                for key, value in address_store.items()
                if value[5] + 300 < current_time  # comparing utime
            ]
            for key in keys_old_entries:
                logger.info(f"(EXPIRED) Remove old address information: {key}")
                address_store.pop(key)
            address_store_checkpoint = current_time

        # pop utime from address_store
        response_data = {key: value[:-1] for key, value in address_store.items()}

    logger.debug(f"(SEND) address_store to client: {client_ip}")
    return web.json_response(response_data)


async def post_address(request):
    client_ip = request.remote
    try:
        data = await request.json()
        ip_address = data.get("ip_address")
        gid = data.get("gid")  # GID of RNIC
        lid = data.get("lid")  # LID of RNIC
        qpn = data.get("qpn")  # QP number
        dtime = data.get("dtime")  # the time that QP is generated at end-host
        utime = time.time()  # the time that server received this post

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
                    logger.critical(
                        "Cleaning up address_store. Check your configuration."
                    )
                    address_store.clear()
            return web.Response(text="Address updated", status=200)
        else:
            logger.warning(f"(RECV) Incorrect POST format from {client_ip}")
            return web.Response(text="Invalid data", status=400)
    except Exception as e:
        logger.error(f"Error processing POST from {client_ip}: {e}")
        return web.Response(text="Internal webserver error", status=500)


# Helper function to read a file’s contents in a thread
def read_file_contents(file_path):
    with open(file_path, "r", encoding="utf-8") as f:
        return f.read()


async def index(request):
    # 1) List files using asyncio.to_thread to avoid blocking
    files = await asyncio.to_thread(os.listdir, HTML_DIR)
    html_files = [f for f in files if f.endswith(".html")]

    # 2) Group the HTML files by protocol -> group -> (fname, measure)
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
          // =========================
          // (1) Clock & Date Utilities
          // =========================

          function formatDateTime(dateObj) {{
            let year = dateObj.getFullYear();
            let month = String(dateObj.getMonth() + 1).padStart(2, '0');
            let day = String(dateObj.getDate()).padStart(2, '0');
            let hour = String(dateObj.getHours()).padStart(2, '0');
            let minute = String(dateObj.getMinutes()).padStart(2, '0');
            let second = String(dateObj.getSeconds()).padStart(2, '0');
            return year + "-" + month + "-" + day + " " + hour + ":" + minute + ":" + second;
          }}

          function updateCurrentTime() {{
            let now = new Date();
            let timeString = formatDateTime(now);
            let currentTimeElem = document.getElementById('currentTime');
            if (currentTimeElem) {{
              currentTimeElem.textContent = timeString;
            }}
          }}
          // Update every second
          setInterval(updateCurrentTime, 1000);

          // ===============================
          // (2) Page Auto-Reload (Optional)
          // ===============================
          function handleAutoReload() {{
            let isEnabled = (localStorage.getItem('autoReloadEnabled') === 'true');
            if (isEnabled) {{
              setInterval(function() {{
                // Re-check user toggle each time
                if (localStorage.getItem('autoReloadEnabled') === 'true') {{
                  window.location.reload();
                }}
              }}, 30000);
            }}
          }}

          // ==================================
          // (3) Protocol & Group Tab Persistence
          // ==================================
          function setupTabPersistence() {{
            // (A) Restore the protocol tab
            let savedProtocolTabId = localStorage.getItem('activeProtocolTab');
            if (savedProtocolTabId) {{
              let tabElement = document.querySelector('#' + savedProtocolTabId);
              if (tabElement) {{
                let protocolTab = new bootstrap.Tab(tabElement);
                protocolTab.show();
              }}
            }}

            // (B) Listen for protocol-tab changes to save new active tab
            const protocolTabEls = document.querySelectorAll('#protocolTab .nav-link');
            protocolTabEls.forEach(el => {{
              el.addEventListener('shown.bs.tab', (event) => {{
                localStorage.setItem('activeProtocolTab', event.target.id);
                // Store also the name of the protocol
                let protocolName = event.target.getAttribute('data-protocol');
                localStorage.setItem('activeProtocolName', protocolName);
              }});
            }});

            // (C) Restore group tab for the current protocol
            let activeProtocolName = localStorage.getItem('activeProtocolName');
            if (activeProtocolName) {{
              let savedGroupTabId = localStorage.getItem('activeGroupTab_' + activeProtocolName);
              if (savedGroupTabId) {{
                let groupTabElement = document.querySelector('#' + savedGroupTabId);
                if (groupTabElement) {{
                  let groupTab = new bootstrap.Tab(groupTabElement);
                  groupTab.show();
                }}
              }}
            }}

            // (D) Listen for group-tab changes, store them per-protocol
            const groupTabEls = document.querySelectorAll('.nav-pills .nav-link');
            groupTabEls.forEach(el => {{
              el.addEventListener('shown.bs.tab', (event) => {{
                let protocolName = el.getAttribute('data-protocol');
                localStorage.setItem('activeGroupTab_' + protocolName, event.target.id);
              }});
            }});
          }}

          // =========================
          // (4) Image Handling
          // =========================

          // (A) Called when an image finishes loading
          function recordLoad(imgElem) {{
            let now = Date.now();
            imgElem.dataset.loadedAt = now;
          }}

          // (B) Update each image's load time every second
          function updateImageTimes() {{
            let now = Date.now();
            let images = document.querySelectorAll('img[data-loaded-at]');
            images.forEach(img => {{
              let loadedTime = parseInt(img.dataset.loadedAt);
              if (!loadedTime) return;
              # let diffSec = Math.floor((now - loadedTime) / 1000);
              # let displayElem = img.parentNode.querySelector('.timeSinceImageLoad');
              # if (displayElem) {{
              #   displayElem.innerText = diffSec + " seconds since this image loaded.";
              # }}
            }});
          }}
          setInterval(updateImageTimes, 1000);

          // (C) Reload all images every 10 seconds
          //     (Separate from page reload)
          function reloadAllImages() {{
            let images = document.querySelectorAll('img[data-original-src]');
            images.forEach(img => {{
              let original = img.dataset.originalSrc;
              // Cache-busting query param
              let newSrc = original + "?_=" + Date.now();
              img.src = newSrc;
            }});
          }}
          setInterval(reloadAllImages, 10000);

          // =========================
          // (5) DOMContentLoaded Setup
          // =========================
          document.addEventListener('DOMContentLoaded', function() {{
            // (A) Set up auto-reload toggle
            let reloadSwitch = document.getElementById('autoReloadSwitch');
            if (reloadSwitch) {{
              let isEnabled = (localStorage.getItem('autoReloadEnabled') === 'true');
              reloadSwitch.checked = isEnabled;
              reloadSwitch.addEventListener('change', () => {{
                localStorage.setItem('autoReloadEnabled', reloadSwitch.checked);
              }});
            }}

            // Kick off the auto-reload logic (page reload)
            handleAutoReload();

            // Set up protocol/group tab persistence
            setupTabPersistence();

            // Trigger the first clock update
            updateCurrentTime();
          }});
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

          <h1 class="mb-3">PingWeave Dashboard</h1>

          <!-- Auto-Reload Toggle Switch (for entire page) -->
          <div class="form-check form-switch mb-3">
            <input class="form-check-input" type="checkbox" id="autoReloadSwitch">
            <label class="form-check-label" for="autoReloadSwitch">
              Auto-Reload Entire Page (every 30s)
            </label>
          </div>

          <p class="text-secondary">
            This PingWeave dashboard shows grouped end-to-end latency monitoring with a mesh grid.<br>
            The images below automatically reload every 10 seconds.
          </p>

          <!-- Protocol Tabs -->
          <ul class="nav nav-tabs" id="protocolTab" role="tablist">
    """

    # Build protocol tabs
    first_protocol = True
    for protocol in protocol_list:
        active_class = "active" if first_protocol else ""
        aria_selected = "true" if first_protocol else "false"
        protocol_button_id = f"tab-{protocol}"
        content += f"""
            <li class="nav-item" role="presentation">
              <button
                class="nav-link {active_class}"
                id="{protocol_button_id}"
                data-bs-toggle="tab"
                data-bs-target="#panel-{protocol}"
                type="button"
                role="tab"
                aria-controls="panel-{protocol}"
                aria-selected="{aria_selected}"
                data-protocol="{protocol}">
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
        show_active_protocol = "show active" if first_protocol else ""
        first_protocol = False

        content += f"""
            <div
              class="tab-pane fade {show_active_protocol}"
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
            group_button_id = f"tab-{safe_group_id}"
            content += f"""
                  <li class="nav-item" role="presentation">
                    <button
                      class="nav-link {active_class}"
                      id="{group_button_id}"
                      data-bs-toggle="pill"
                      data-bs-target="#panel-{safe_group_id}"
                      type="button"
                      role="tab"
                      aria-controls="panel-{safe_group_id}"
                      aria-selected="{aria_selected}"
                      data-protocol="{protocol}">
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
            show_active_group = "show active" if first_group else ""
            first_group = False
            safe_group_id = f"{protocol}-{group_name}".replace(" ", "_")

            file_list = group_dict[group_name]
            # Sort by measure
            file_list = sorted(file_list, key=lambda x: x[1])

            content += f"""
                  <div
                    class="tab-pane fade {show_active_group}"
                    id="panel-{safe_group_id}"
                    role="tabpanel"
                    aria-labelledby="tab-{safe_group_id}">

                    <h5 class="mb-3">Group: {group_name} ({len(file_list)} files)</h5>
                    <ul class="list-unstyled">
            """
            for fname, measure in file_list:
                # Attempt to load summary text
                summary_fname = fname.replace(".html", ".summary")
                summary_path = os.path.join(SUMMARY_DIR, summary_fname)

                summary_text = "No summary available."
                summary_timestamp_html = ""

                if await asyncio.to_thread(os.path.exists, summary_path):
                    loaded_text = await asyncio.to_thread(
                        read_file_contents, summary_path
                    )
                    summary_text = loaded_text
                    loaded_timestamp = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
                    summary_timestamp_html = f"""
                        <div class="text-muted small">
                          Summary loaded at: {loaded_timestamp}
                        </div>
                    """

                summary_text_escaped = html.escape(summary_text)

                # Prepare PNG path
                # We assume the PNG has the same base name as the HTML file,
                # e.g., "roce_group_something_measure1_measure2.png"
                png_fname = fname.replace(".html", "")
                png_path = f"/image/{png_fname}.png"

                content += f"""
                      <li class="mb-4 border-bottom pb-3">
                        <div><strong>Measure:</strong> {measure}</div>
                        <div><strong>Filename:</strong>
                          <a href="/{fname}" target="_blank">{fname}</a>
                        </div>

                        <div class="d-flex mt-3">
                          <div>
                            <img src="{png_path}"
                                data-original-src="{png_path}"
                                alt="{png_fname}"
                                onload="recordLoad(this)"
                                data-loaded-at=""
                                style="max-width:600px; border:1px solid #aaa; border-radius:3px;">
                          </div>
                          <div class="ms-3" style="flex: 1;">
                            <div class="card shadow-sm" style="min-width:300px;">
                              <div class="card-header bg-primary text-white">
                                Summary
                              </div>
                              <div class="card-body">
                                <p class="card-text" style="white-space: pre-wrap;">{summary_text_escaped}</p>
                                {summary_timestamp_html}
                              </div>
                            </div>
                          </div>
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
      </body>
    </html>
    """

    return web.Response(text=content, content_type="text/html")


async def pingweave_webserver():
    try:
        while True:
            try:
                app = web.Application()
                app.router.add_get("/", index)  # indexing for html files
                app.router.add_static("/", HTML_DIR)  # static route for html files
                app.router.add_static("/summary", SUMMARY_DIR)  # static for summary
                app.router.add_static("/image", IMAGE_DIR)  # static route for images
                app.router.add_static("/bootstrap", BOOTSTRAP_DIR)  # for bootstrap
                app.router.add_get("/pinglist", get_pinglist)
                app.router.add_get("/address_store", get_address_store)
                app.router.add_post("/address", post_address)

                runner = web.AppRunner(app)
                await runner.setup()

                site = web.TCPSite(runner, host="0.0.0.0", port=config["control_port"])
                await site.start()

                logger.info(
                    f"Pingweave webserver running on {config['control_host']}:{config['control_port']}"
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
    setproctitle("pingweave_ctrl_webserver.py")
    try:
        asyncio.run(pingweave_webserver())
    except KeyboardInterrupt:
        logger.info("pingweave_webserver process received KeyboardInterrupt. Exiting.")
