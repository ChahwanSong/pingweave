# PingWeave
PingWeave

## Prerequisites
### Nodes
* python >= 3.6 (for asyncio)
* ibverbs library (e.g., libibverbs) - rdma-core-dev
* python3 -m pip install logging

### Control plane
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
* python packages
    * pyyaml
    * psutil
    * pandas
    * numpy
    * plotly
    * aiohttp
    * redis (client)
    * logging

