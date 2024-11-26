## Redis server
Ubuntu 22에서 apt install redis로 Redis를 설치한 후, Unix Domain Socket을 활성화하려면 설정 파일(redis.conf)을 수정하고 Redis를 재시작하면 됩니다. 아래는 단계별 가이드입니다.

1. Redis 설치 확인
```
sudo apt install redis
```
Redis가 설치되었는지 확인:

```
redis-cli --version
```
Redis 서버 상태 확인:

```
sudo systemctl status redis
```
2. 설정 파일 위치 확인
APT로 설치된 Redis의 기본 설정 파일은 /etc/redis/redis.conf에 위치합니다.

```
sudo nano /etc/redis/redis.conf
```
3. Unix Domain Socket 활성화
Redis 설정 파일에서 다음 항목을 수정합니다:

1. Unix Socket 경로 설정
unixsocket 항목을 활성화하고 경로를 지정합니다:

```
unixsocket /var/run/redis/redis-server.sock
```
2. Unix Socket 권한 설정
unixsocketperm 항목을 활성화하고 권한을 지정합니다. 일반적으로 700 또는 770을 사용합니다:

```
unixsocketperm 700
```
3. TCP/IP 연결 비활성화 (선택 사항)
만약 Unix Domain Socket만 사용하려면 TCP 포트를 비활성화합니다:

```
port 0
```

4. Redis 소켓 디렉토리 권한 설정
Unix Socket 파일을 저장하는 디렉토리(/var/run/redis/)가 Redis 사용자에 의해 읽기/쓰기 가능해야 합니다.

디렉토리 확인:

```
ls -ld /var/run/redis/
```
필요시 디렉토리 권한 수정:

```
sudo mkdir -p /var/run/redis
sudo chown redis:redis /var/run/redis
sudo chmod 770 /var/run/redis
```
5. Redis 서비스 재시작
설정을 적용하려면 Redis를 재시작합니다:

```
sudo systemctl restart redis
```
6. Unix Domain Socket 테스트
Redis가 Unix Domain Socket을 통해 작동하는지 확인합니다.

Redis CLI에서 Unix Socket 사용:

```
redis-cli -s /var/run/redis/redis-server.sock
```
정상 작동 시 Redis CLI 프롬프트로 이동합니다:

```
127.0.0.1:6379> PING
PONG
```

7. 추가 팁: Unix Domain Socket과 TCP/IP 병행
만약 Unix Domain Socket과 TCP/IP를 모두 사용하고 싶다면:

TCP 포트를 그대로 유지 (port 6379).
Unix Domain Socket을 활성화하여 두 통신 방식을 병행 사용할 수 있습니다.
요약
Redis 설정 파일(/etc/redis/redis.conf)에서 다음과 같이 변경하세요:

```
unixsocket /var/run/redis/redis-server.sock
unixsocketperm 700
port 0  # TCP/IP 비활성화 (선택 사항)
```
Redis 재시작 후 Unix Domain Socket이 활성화되었는지 확인하고 사용하면 됩니다.


로그확인
```
sudo tail -f /var/log/redis/redis-server.log
```


### Permission Denied
```
sudo usermod -aG redis mason
sudo chmod 770 /var/run/redis/redis-server.sock
sudo chown redis:redis /var/run/redis/redis-server.sock
```
또는 설정파일 `/etc/redis/redis.conf` 들어가서
```
unixsocketperm 770
```

설정 변경 후 재시작
```
sudo systemctl restart redis
grep unixsocket /etc/redis/redis.conf
```

테스트
```
redis-cli -s /var/run/redis/redis-server.sock
```




## Redis client (python)

1. 설치
Redis 클라이언트를 설치하려면 redis-py 패키지를 설치해야 합니다.

```
pip install redis
```
2. 기본 연결
Redis 서버에 연결하려면 redis.StrictRedis 또는 redis.Redis 클래스를 사용합니다.

TCP 연결 (기본 설정)
```
import redis

# Redis 클라이언트 생성
r = redis.StrictRedis(host='localhost', port=6379, decode_responses=True)

# PING 테스트
print(r.ping())  # 출력: True
```


Unix Domain Socket 연결

```
import redis

# Unix Domain Socket 경로
r = redis.StrictRedis(unix_socket_path='/var/run/redis/redis-server.sock', decode_responses=True)

# PING 테스트
print(r.ping())  # 출력: True
```
3. 기본 명령
Redis의 주요 명령을 Python 코드로 실행하는 방법입니다.

문자열 데이터 저장 및 읽기
```
# 데이터 저장
r.set('key', 'value')

# 데이터 읽기
value = r.get('key')
print(value)  # 출력: 'value'
```

해시(HASH) 데이터 사용
```
# 해시 저장
r.hset('myhash', 'field1', 'value1')
r.hset('myhash', 'field2', 'value2')

# 해시 읽기
print(r.hget('myhash', 'field1'))  # 출력: 'value1'
print(r.hgetall('myhash'))  # 출력: {'field1': 'value1', 'field2': 'value2'}
```

리스트(LIST) 사용

```
# 리스트 추가
r.rpush('mylist', 'item1', 'item2', 'item3')

# 리스트 읽기
print(r.lrange('mylist', 0, -1))  # 출력: ['item1', 'item2', 'item3']
```

집합(SET) 사용
```
# 집합 추가
r.sadd('myset', 'item1', 'item2', 'item3')

# 집합 읽기
print(r.smembers('myset'))  # 출력: {'item1', 'item2', 'item3'}
```

4. Pub/Sub (Publish/Subscribe)
메시지 발행 (Publisher)

```
import redis

r = redis.StrictRedis(host='localhost', port=6379, decode_responses=True)

# 메시지 발행
r.publish('channel', 'Hello, Subscribers!')
```

메시지 구독 (Subscriber)

```
import redis

r = redis.StrictRedis(host='localhost', port=6379, decode_responses=True)

# 메시지 구독
pubsub = r.pubsub()
pubsub.subscribe('channel')

print('Subscribed to channel...')

# 메시지 수신
for message in pubsub.listen():
    if message['type'] == 'message':
        print(f"Received: {message['data']}")
```

5. 연결 풀 (Connection Pool)
Redis 클라이언트 연결을 효율적으로 관리하려면 Connection Pool을 사용할 수 있습니다.

```
from redis import ConnectionPool, StrictRedis

# 연결 풀 생성
pool = ConnectionPool(host='localhost', port=6379, decode_responses=True)

# Redis 클라이언트에 연결 풀 사용
r = StrictRedis(connection_pool=pool)

# 데이터 저장 및 읽기
r.set('key', 'value')
print(r.get('key'))  # 출력: 'value'
```


6. 예외 처리
Redis 서버 연결이나 명령 실행 중 에러를 처리하려면 예외를 다뤄야 합니다.

```
import redis

try:
    r = redis.StrictRedis(host='localhost', port=6379, decode_responses=True)
    r.ping()
    print("Connected to Redis!")
except redis.ConnectionError as e:
    print(f"Redis connection failed: {e}")
```