## TODO

- main 함수 멀티 프로세싱
    - 4개 fork() 멀티프로세스로 리소스-안정적으로 실행 (restart 기능)
- log initialize 가 자꾸 발생함
    - 개별 logger로 해결
- server가 핑 16개 이상 받고는, 그이상은 WQ 안뽑아냄
    - ibv_next 의 batch size = 16. 대신, ibv_start_poll, ibv_end_poll 사용
- ping, pong message 구조체
    - Done
- Timestamp Bit overflow 이슈 (uint64_t)
    - Done
- Inter-thread message queue 오버헤드 (tail latency) - slower than RDMA latency
    - mutex 로 해결
- Out-of-order PONG / ACK
    - Fix 함
- Buffer override
    - Event-driven & small jittering 으로 해결
- spdlog의 logger loading latency 이슈
    - logname 말고 Logger 를 패스하게 함.
- infiniband support
    - LID 포함 (GID, LID, QPN) 시키기 -> 타입 string, string, integer 캐스팅.
- Traffic class = 3
    - post_send() 의 Grh 에 설정
- Merge python client/server to pingweave.cpp
    - Done
- 모든 ping 실패 시 statistics -> 0 으로 표시되게 하기
- server 에서 message 도착을 전부 캐치 못하고 놓침
    - recv_post 를 처음에 많이 해두기? 그럼 buffer overlaid 문제 발생?
- Recv/Send buffer 여러개 사용하기 (buffer corruption 문제)
    - Done
- YAML C++ LIBRARY  -> use fkyaml library
    - Done
- HTTP Server - aiohttp
    - Done (pingweave_server.py)
- collector 에서 /alarm 처리기능 추가 (alarm 시 logging 기능)
    - Done
- pignweavecpp 에서 새로운 실행, 또는 processor 종료 시 collector 에 메시지 보내기
    - Done
- 데이터베이스
    - Pingweave Server 노드에서 REDIS 서버 실행. 
    - Redis 설정에서 Redis 설정 파일에서 Unix Domain Socket 활성화 시키기 -> single node 에서 Pub/sub 에 효율적
    - Collector 에서 받은 Ping 결과 publish.
    - database.py -> subscribe 해서 Database 에 저장
    - analyzer.py -> pinglist format을 기반으로 
- Broadcom RNIC 지원 (no support for HW timestamping)
    -

## Overhead
* CPU: Intel(R) Xeon(R) Gold 6326 CPU @ 2.90GHz
* 2 node - (1) non-hyperthreading, (2) hyperthreading
* pingweave has two processes - (1) TX, (2) RX
### ping 2/second for TX, 2/second for RX
- non-hyperthreading
    mason    2841856  5.5  0.0 161552  7180 pts/5    Sl+  06:00   0:01 ../bin/pingweave
    mason    2841857  5.5  0.0 236340  7176 pts/5    Sl+  06:00   0:01 ../bin/pingweave
- hyperthreading
    mason    2413068  6.9  0.0 161556  7144 pts/0    Sl+  06:00   0:03 ../bin/pingweave
    mason    2413069  7.2  0.0 236340  7184 pts/0    Sl+  06:00   0:03 ../bin/pingweave
### ping ~4K/second for TX, ~4K/second for RX
- non-hyperthreading
    mason    2814898 14.9  0.1 554772 410512 pts/0   Sl+  05:38   0:48 ../bin/pingweave
    mason    2814899 17.3  0.0 432944 218112 pts/0   Sl+  05:38   0:55 ../bin/pingweaveccccccccc
- hyperthreading
    mason     257428 24.3  0.1 554772 409000 pts/0   Sl+  05:38   1:20 ../bin/pingweave
    mason     257429 36.6  0.0 432948 203864 pts/0   Sl+  05:38   2:01 ../bin/pingweave
### ping ~10K/second for TX, ~10K/second for RX
- non-hyperthreading
    mason    2815575 25.7  0.4 1210128 1093172 pts/0 Sl+  07:16   1:40 ../bin/pingweave
    mason    2815576 33.9  0.2 891700 700880 pts/0   Sl+  07:16   2:13 ../bin/pingweave
- hyperthreading
    mason     406627 45.4  0.2 751380 636472 pts/0   Sl+  07:19   1:30 ../bin/pingweave
    mason     406628 56.7  0.1 498484 297340 pts/0   Sl+  07:19   1:53 ../bin/pingweave


## Install Redis

```
sudo vi /etc/redis/redis.conf
```

Redis 설정 파일에서 Unix Domain Socket 활성화하기.
Unix Domain Socket은 로컬 통신에만 사용할 수 있습니다.
원격 접근이 필요하면 TCP 포트(port 6379)를 함께 활성화해야 합니다.
/tmp/redis.sock 경로는 시스템 재부팅 시 초기화될 수 있으므로, 더 안정적인 경로(예: /var/run/redis/redis.sock)를 사용하는 것이 좋습니다.

Reference: https://lhr0419.medium.com/%EB%A0%88%EB%94%94%EC%8A%A4%EC%97%90-%EB%8C%80%ED%95%9C-%EA%B0%84%EB%8B%A8%ED%95%9C-%EC%84%A4%EB%AA%85%EA%B3%BC-%EC%9A%B4%EC%98%81%ED%8C%81-42a52dd71b3e

```
redis-cli -s /var/run/redis/redis-server.sock
```

```python
import redis

# Unix Domain Socket 경로
r = redis.StrictRedis(unix_socket_path='/var/run/redis/redis-server.sock', decode_responses=True)

# PING 테스트
print(r.ping())  # 출력: True
```
