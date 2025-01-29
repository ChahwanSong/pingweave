import zmq
import threading


def worker_routine(worker_url, worker_id):
    """
    각 워커 스레드에서 실행되는 함수.
    DEALER 소켓으로 서버(backend)와 통신.
    """
    context = zmq.Context.instance()
    socket = context.socket(zmq.DEALER)
    socket.connect(worker_url)

    while True:
        # ROUTER -> (proxy) -> DEALER 로부터 multipart 메시지 수신
        # 기본적으로 [클라이언트ID, 빈프레임, 실제데이터] 형태(3개 프레임)로 들어올 것이라 가정
        frames = socket.recv_multipart()

        # 안정적으로 처리하기 위해, 프레임 수 확인
        if len(frames) == 3:
            client_id, empty_frame, client_msg = frames
        else:
            # (예외 처리) 예상치 못한 형식이면 로그 출력 후 스킵
            print(f"[Worker {worker_id}] Unexpected frames: {frames}")
            continue

        # 클라이언트 ID는 사람이 읽기 어려울 수 있으므로 hex로 변환(선택)
        client_id_str = client_id.decode(
            "utf-8", errors="replace"
        )  # 혹은 client_id.hex()
        # 메시지도 UTF-8이 아닐 수 있으므로 안전 처리
        client_msg_str = client_msg.decode("utf-8", errors="replace")

        print(f"[Worker {worker_id}] from <{client_id_str}> received: {client_msg_str}")

        # 여기서 실제 로직(연산, DB저장 등)을 수행할 수 있음
        # ...

        # 응답 전송: [클라이언트ID, 빈프레임, 응답데이터]
        reply_data = f"Reply from worker {worker_id}".encode("utf-8")
        socket.send_multipart([client_id, b"", reply_data])


def main():
    context = zmq.Context.instance()

    # 1) 클라이언트들이 연결할 ROUTER 소켓(frontend)
    frontend = context.socket(zmq.ROUTER)
    frontend.bind("tcp://10.200.200.3:5555")

    # 2) 워커들과 통신할 DEALER 소켓(backend, inproc)
    backend = context.socket(zmq.DEALER)
    backend.bind("inproc://workers")

    # 3) 워커 스레드 4개 생성
    num_workers = 4
    for i in range(num_workers):
        t = threading.Thread(
            target=worker_routine, args=("inproc://workers", i), daemon=True
        )
        t.start()

    # 4) frontend(ROUTER) <-> backend(DEALER)을 중개
    zmq.proxy(frontend, backend)

    # 종료 처리(일반적으로 여기 도달 안 함)
    frontend.close()
    backend.close()
    context.term()


if __name__ == "__main__":
    main()
