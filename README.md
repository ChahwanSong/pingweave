# PingWeave
PingWeave is a mesh-grid network monitoring framework for various transport protocols. 
It currently supports TCP, UDP, RDMA (RoCEv2 and Infiniband). 
The design goals are as following:
(1) Performant: CPU consumption, OS resources such as connection states.
(2) Scalable: Support ~O(10K) pings per second
(3) Standalone: Minimum package dependency (C++, python)  
(4) Versatile: Supports RDMA, TCP, UDP (and more in future)

Specifically, pingweave's RDMA ping framework is based on unreliable datagram (UD) to minimize the connection overhead.
Pingweave supports RDMA HW timestamping as long as RNIC can provide. That said, it supports ns-scale high-resolution ping monitoring.

Currently, `linux` is the only OS that pingweave supports. It is tested on RHEL 8/9 and Ubuntu 22.04.  

---

## Simple Installation
**IMPORTANT** We assume to run `pingweave` on `root`. 


Basically, it has two types of nodes:
* Agent: a node to send/recv pings
* Controller: a control plane that distributes a pinglist, collets ping results, and host a webserver for plots.

You can easily install `pingweave` using our script (see (scripts/install.sh)[scripts/install.sh]).
If one of the preliminaries (see below) are not ready, the script exits with error messages.

Here are some example commands:
```shell
# agent
scripts/install.sh 
# controller
scripts/install.sh -c
# help
scripts/install.sh -h
# uninstall the pingweave service
scripts/install.sh -d
```

#### RDMA library
By default, it checks the ibverb library (e.g., `rdma-core`) and install if it does not exist. 

#### Custom `pypi` and `pip.conf`
If your cluster is offline and uses a private pypi cluster, you may want to update your `pip.conf` before installing python packages. 
This is simply done by updating your custom [scripts/pip.conf](scripts/pip.conf) and run `install.sh` with `-p` flag:
```shell
# agent
scripts/install.sh -p
# controller
scripts/install.sh -c -p
```

#### Resource hard-limits 
For a stable service, we put some resource limits in [scripts/pingweave.service](pingweave.service) as following:
```shell
# CPU and memory resource limit
CPUQuota=600%
MemoryMax=4G

# file and process limit
LimitNOFILE=4096
LimitNPROC=64
```  
Although this overhead is enough to send/recv tens of thousands of ping messages in state-of-the-art servers, you may want to change them to adapt pingweave to your cluster. 



## Preliminaries
Before running `pingweave`, it requires a few preliminaries. 

### Common
Both agent and controller need what follows:
* `c++17`
* `libibverbs` (ibverbs library): `rdma-core` or `rdma-core-devel`.
* `chronyd.service` (NTP time synchronization): Time-sync is used to collect data and analyze the ping results. It is also used to evict stale ping results. 

### Agent
Agent requires the following python packages (see [scripts/requirements_agent.txt](scripts/requirements_agent.txt)):
* python >= 3.6 (for `asyncio`)
* python packages
    * `pyzmq`
    * `datetime`
    * `setproctitle`
    * `pyyaml`

### Controller
Controller requires the following python pacakges (see [scripts/requirements_controller.txt](scripts/requirements_controller.txt)):
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

In addition, the controller runs in-memory key-value store to keep the "very recent" ping results as a mesh-grid data structure. 
On RHEL, you can install the redis:
```shell
dnf install redis -y 
systemctl start redis
systemctl status redis
```

The information stored in redis is used for plotting figures (see [src/ctrl_plotter.py](src/ctrl_plotter.py)).
As the plotting process is running in the same node, we use a unix domain socket for local communications.
To use unix domain socket, you can modify a redis config file (`/etc/redis.conf` for RHEL, and `/etc/redis/redis.conf` for Ubuntu):
```yaml
unixsocket /var/run/redis/redis-server.sock # specify the path to unix socket
unixsocketperm 700 # usually 700 or 770
```
Then restart the redis service.





## Known Issues
### Inconsistent RNIC timestamping for heterogeneous RNICs 
When pinging RDMA among different types of RDMA NICs (e.g., CX6 and CX-4), we observe that the time clocking is not consistent.
We classify such events to "werid" class, get and plot the statistics.  

### RNIC HW time fluctuation

### Non-root installation



<!-- 
### pingweavectl
```
sudo cp $SCRIPT_DIR/scripts/pingweavectl /usr/local/bin
```

####
Test codes

```
ps -eo pid,args,comm,rss,vsz --sort=-rss | awk '/pingweave/ {printf "PID: %s, ARGS: %s, COMMAND: %s, RSS: %.2f MB, VSZ: %.2f MB\n", $1, $2, $3, $4/1024, $5/1024}'
```

## TO-DO List
* Infiniband -> DHCP 0, RoCEv2 -> DHCP 106 


## redis key

```
redis-cli -s /var/run/redis/redis-server.sock keys '*' | while read key; do echo "$key => $(redis-cli -s /var/run/redis/redis-server.sock get "$key")"; done
```

-->
