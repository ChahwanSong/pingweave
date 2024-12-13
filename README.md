# PingWeave
PingWeave

## Prerequisites
### Nodes
* python >= 3.6 (for asyncio)
* ibverbs library (e.g., libibverbs) - rdma-core-dev for RHEL.

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

### Install & Build



### pingweavectl
`sudo cp $SCRIPT_DIR/scripts/pingweavectl /usr/local/bin`
