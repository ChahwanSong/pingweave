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



## Overhead
* CPU: Intel(R) Xeon(R) Gold 6326 CPU @ 2.90GHz
* 2 node - (1) non-hyperthreading, (2) hyperthreading
* pingweave has two processes - (1) TX, (2) RX
### ping ~4K/second for TX, ~4K/second for RX
- non-hyperthreading
    mason    2814898 14.9  0.1 554772 410512 pts/0   Sl+  05:38   0:48 ../bin/pingweave
    mason    2814899 17.3  0.0 432944 218112 pts/0   Sl+  05:38   0:55 ../bin/pingweave
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
