# Redis Server Setup and Configuration on Ubuntu 22.04

After installing Redis on Ubuntu 22.04 using `apt install redis`, you can enable Unix Domain Socket support by modifying the Redis configuration file (`redis.conf`) and restarting the Redis service. Below is a step-by-step guide:

---

## 1. Verify Redis Installation

Install Redis:

```bash
sudo apt install redis
```

Check if Redis is installed:

```bash
redis-cli --version
```

Verify the Redis server status:

```bash
sudo systemctl status redis
```

---

## 2. Locate the Configuration File

The default configuration file for Redis (installed via APT) is located at `/etc/redis/redis.conf`:

```bash
sudo nano /etc/redis/redis.conf
```

---

## 3. Enable Unix Domain Socket

### Update the following settings in the Redis configuration file:

1. **Set the Unix Socket Path**  
Uncomment and specify the path for the `unixsocket` entry:

```bash
unixsocket /var/run/redis/redis-server.sock
```

2. **Set Unix Socket Permissions**  
Uncomment and specify permissions for the `unixsocketperm` entry (commonly `700` or `770`):

```bash
unixsocketperm 700
```

3. **Disable TCP/IP (Optional)**  
If you want to use only Unix Domain Socket, disable the TCP port:

```bash
port 0
```

---

## 4. Adjust Socket Directory Permissions

The directory where the Unix socket file is stored (`/var/run/redis/`) must be readable and writable by the Redis user.

Check the directory:

```bash
ls -ld /var/run/redis/
```

Modify directory permissions if needed:

```bash
sudo mkdir -p /var/run/redis
sudo chown redis:redis /var/run/redis
sudo chmod 770 /var/run/redis
```

---

## 5. Restart the Redis Service

Apply the changes by restarting Redis:

```bash
sudo systemctl restart redis
```

---

## 6. Test the Unix Domain Socket

Check if Redis is working via the Unix Domain Socket:

```bash
redis-cli -s /var/run/redis/redis-server.sock
```

If successful, you will see the Redis CLI prompt:

```bash
127.0.0.1:6379> PING
PONG
```

---

## 7. Using Unix Domain Socket with TCP/IP

To use both Unix Domain Socket and TCP/IP together:
- Keep the TCP port enabled (`port 6379`).
- Enable Unix Domain Socket to allow both methods simultaneously.

---

## 8. Summary of Configuration Changes

In `/etc/redis/redis.conf`, make the following updates:

```bash
unixsocket /var/run/redis/redis-server.sock
unixsocketperm 700
port 0  # Optional: Disable TCP/IP
```

After restarting Redis, verify that Unix Domain Socket is active and in use.

---

## 9. Check Logs

Monitor Redis logs to debug any issues:

```bash
sudo tail -f /var/log/redis/redis-server.log
```

---

## 10. Fix Permission Denied Issues

If you encounter a permission issue, try the following:

```bash
sudo usermod -aG redis mason
sudo chmod 770 /var/run/redis/redis-server.sock
sudo chown redis:redis /var/run/redis/redis-server.sock
```

Alternatively, update the `unixsocketperm` value in `/etc/redis/redis.conf`:

```bash
unixsocketperm 770
```

Restart Redis:

```bash
sudo systemctl restart redis
grep unixsocket /etc/redis/redis.conf
```

---

## 11. Test Again

Test Unix Domain Socket connectivity:

```bash
redis-cli -s /var/run/redis/redis-server.sock
```

---

# Redis Client (Python)

## 1. Installation

Install the `redis-py` package:

```bash
pip install redis
```

---

## 2. Basic Connection

### Using TCP (Default)

```python
import redis

# Create Redis client
r = redis.StrictRedis(host='localhost', port=6379, decode_responses=True)

# Test connection
print(r.ping())  # Output: True
```

### Using Unix Domain Socket

```python
import redis

# Connect via Unix Domain Socket
r = redis.StrictRedis(unix_socket_path='/var/run/redis/redis-server.sock', decode_responses=True)

# Test connection
print(r.ping())  # Output: True
```

---

## 3. Basic Commands

### Storing and Retrieving Strings

```python
# Store data
r.set('key', 'value')

# Retrieve data
value = r.get('key')
print(value)  # Output: 'value'
```

### Working with Hashes

```python
# Store hash
r.hset('myhash', 'field1', 'value1')
r.hset('myhash', 'field2', 'value2')

# Retrieve hash
print(r.hget('myhash', 'field1'))  # Output: 'value1'
print(r.hgetall('myhash'))  # Output: {'field1': 'value1', 'field2': 'value2'}
```

### Using Lists

```python
# Add to list
r.rpush('mylist', 'item1', 'item2', 'item3')

# Retrieve list
print(r.lrange('mylist', 0, -1))  # Output: ['item1', 'item2', 'item3']
```

### Using Sets

```python
# Add to set
r.sadd('myset', 'item1', 'item2', 'item3')

# Retrieve set
print(r.smembers('myset'))  # Output: {'item1', 'item2', 'item3'}
```

---

## 4. Pub/Sub (Publish/Subscribe)

### Publish Messages

```python
import redis

r = redis.StrictRedis(host='localhost', port=6379, decode_responses=True)

# Publish a message
r.publish('channel', 'Hello, Subscribers!')
```

### Subscribe to Messages

```python
import redis

r = redis.StrictRedis(host='localhost', port=6379, decode_responses=True)

# Subscribe to channel
pubsub = r.pubsub()
pubsub.subscribe('channel')

print('Subscribed to channel...')

# Receive messages
for message in pubsub.listen():
    if message['type'] == 'message':
        print(f"Received: {message['data']}")
```

---

## 5. Using Connection Pools

```python
from redis import ConnectionPool, StrictRedis

# Create connection pool
pool = ConnectionPool(host='localhost', port=6379, decode_responses=True)

# Use connection pool with Redis client
r = StrictRedis(connection_pool=pool)

# Store and retrieve data
r.set('key', 'value')
print(r.get('key'))  # Output: 'value'
```

---

## 6. Exception Handling

```python
import redis

try:
    r = redis.StrictRedis(host='localhost', port=6379, decode_responses=True)
    r.ping()
    print("Connected to Redis!")
except redis.ConnectionError as e:
    print(f"Redis connection failed: {e}")
```