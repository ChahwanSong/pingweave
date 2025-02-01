import zmq
import time
import statistics


def main():
    context = zmq.Context()

    # 클라이언트 소켓: DEALER
    socket = context.socket(zmq.DEALER)

    # (선택) 클라이언트 식별자(Identity)를 텍스트로 지정
    # 지정하지 않으면 랜덤 바이너리 ID가 붙음
    client_id = b"client-1"
    socket.setsockopt(zmq.IDENTITY, client_id)

    # 서버 ROUTER("tcp://*:5555")로 연결
    socket.connect("tcp://10.200.200.3:5555")

    # poller 등록: POLLIN(읽기 가능 이벤트) 관찰
    poller = zmq.Poller()
    poller.register(socket, zmq.POLLIN)

    num_messages = 10  # 전송할 메시지 수
    latencies = []  # 각 메시지별 레이턴시(ms)

    for i in range(num_messages):
        # 전송할 메시지(UTF-8 인코딩)
        msg_text = f"Hello {i}" * 1000
        msg_bytes = msg_text.encode("utf-8")

        # 전송 직전 시간 기록
        start_time = time.perf_counter()

        # DEALER -> ROUTER 로 멀티파트 전송: [빈프레임, 실제메시지]
        #  - ROUTER 소켓은 첫 프레임에 클라이언트 ID를 자동으로 추가/인식하지 않음(REQ/REP와 다름)
        #  - 그러나 "proxy" 환경에서 ROUTER는 클라이언트 ID를 별도 프레임으로 유지해준다.
        #  - 관례상, "빈 프레임" + "메시지" 형태로 보내면,
        #    서버의 워커에서 [클라이언트ID, 빈프레임, 메시지] 로 받게 된다.
        socket.send_multipart([b"HELLO", msg_bytes])
        print(f"[Client] Sent: {msg_text}")

        # 응답 대기 (5초 타임아웃)
        timeout_ms = 5000
        socks = dict(poller.poll(timeout_ms))
        if socket in socks and socks[socket] == zmq.POLLIN:
            # 응답이 도착했으므로, 수신
            # 전형적으로 [서버(워커)ID, 빈프레임, 응답메시지] 형태일 것
            reply_frames = socket.recv_multipart(zmq.NOBLOCK)

            # 마지막 프레임이 실제 응답 메시지라고 가정
            reply_msg = reply_frames[-1]
            reply_text = reply_msg.decode("utf-8", errors="replace")

            # 레이턴시 계산
            end_time = time.perf_counter()
            latency_ms = (end_time - start_time) * 1000
            latencies.append(latency_ms)

            print(f"[Client] Received: {reply_text}, Latency: {latency_ms:.3f} ms")
        else:
            # 타임아웃
            print("[Client] Timed out waiting for reply")

        time.sleep(0.5)

    # 전체 메시지에 대한 통계
    if latencies:
        avg_latency = statistics.mean(latencies)
        stdev_latency = statistics.pstdev(latencies)  # 표본 표준편차
        print("\n--- Latency Statistics ---")
        print(f"Total messages sent: {num_messages}")
        print(f"Replies received: {len(latencies)}")
        print(f"Average latency: {avg_latency:.3f} ms")
        print(f"StdDev latency: {stdev_latency:.3f} ms")
    else:
        print("\nNo successful replies (all timed out).")

    socket.close()
    context.term()


if __name__ == "__main__":
    main()
