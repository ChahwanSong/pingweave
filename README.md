# PingWeave
PingWeave

## Prerequisites
### Common
* `c++17`
* `libibverbs` (ibverbs library): `rdma-core` or `rdma-core-devel`
* `chronyd.service` (NTP time synchronization): to evict stale information 

### Agent
* python >= 3.6 (for asyncio)
* python packages
    * pyzmq
    * datetime
    * setproctitle
    * pyyaml

### Controller
* redis: in-memory key-value store
```shell
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
    * aiohttp
    * numpy
    * redis
    * jinja2
    * datetime
    * setproctitle
    * plotly
    * kaleido
    * pyzmq
    * matplotlib


For python package installations, see [scripts/requirements_agent.txt](scripts/requirements_agent.txt) and [scripts/requirements_controller.txt](scripts/requirements_controller.txt).


### Install & Build



### pingweavectl
```
sudo cp $SCRIPT_DIR/scripts/pingweavectl /usr/local/bin
```

####
Test codes

```
ps -eo pid,args,comm,rss,vsz --sort=-rss | awk '/pingweave/ {printf "PID: %s, ARGS: %s, COMMAND: %s, RSS: %.2f MB, VSZ: %.2f MB\n", $1, $2, $3, $4/1024, $5/1024}'
```

```
../bin/pingweave_simple -a 10.200.200.3 --tcp -s
../bin/pingweave_simple -a 10.200.200.2 --tcp -c
```

## Reconfiguration

- Client 
    - just change `config/pingweave.ini` file.
- Server
    - Restart (for now)


## TO-DO List
* Infiniband -> DHCP 0, RoCEv2 -> DHCP 106