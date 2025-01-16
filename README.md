# PingWeave
PingWeave

## Prerequisites

### Common
* c++17
* ibverbs library (e.g., libibverbs) - "rdma-core" or "rdma-core-devel"
* chronyd.service (NTP time synchronization) - to evict stale information 

### Nodes
* python >= 3.6 (for asyncio)

### Controller
* redis: in-memory key-value store
```
dnf install redis -y
systemctl start redis
systemctl status redis
```
Modify redis config file `/etc/redis.conf`:
```
unixsocket /var/run/redis/redis-server.sock
unixsocketperm 700
port 0  # TCP/IP inactivate (optional)
```

* python >= 3.7 (for aiohttp)
* python packages (versions do not necessarily be same with clients)
    * pyyaml
    * psutil
    * pandas
    * numpy
    * plotly
    * aiohttp (+ jinja2)
    * redis (client)
    * logging
    * datetime
    * setproctitle
    * kaleido

### Install & Build



### pingweavectl
```
sudo cp $SCRIPT_DIR/scripts/pingweavectl /usr/local/bin
```


```
ps -eo pid,args,comm,rss,vsz --sort=-rss | awk '/pingweave/ {printf "PID: %s, ARGS: %s, COMMAND: %s, RSS: %.2f MB, VSZ: %.2f MB\n", $1, $2, $3, $4/1024, $5/1024}'
```

## Reconfiguration

- Client 
    - just change `config/pingweave.ini` file.
- Server
    - Restart (for now)


## TO-DO List
* Infiniband -> DHCP 0, RoCEv2 -> DHCP 106