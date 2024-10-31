import os
import asyncio
import sys
import subprocess

try:
    import aiohttp
except ImportError:
    print("Module 'aiohttp' is not found. Installing...")
    subprocess.check_call([sys.executable, "-m", "pip", "install", "aiohttp"])
    import aiohttp

import asyncio
from aiohttp import web
import yaml

# 스크립트와 같은 디렉토리에 있는 pinglist.yaml 파일의 절대 경로를 설정
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
PINGLIST_FILE = os.path.join(SCRIPT_DIR, "pinglist.yaml")

# 포트 번호 매크로 선언
PINGLIST_PORT = 33335

# 메모리에 저장된 pinglist 데이터를 담을 변수
data_in_memory = {}
data_lock = asyncio.Lock()


# 60초마다 pinglist 파일을 읽어 메모리에 로드하는 함수
async def load_pinglist_periodically():
    global data_in_memory

    while True:
        try:
            async with (
                data_lock
            ):  # 락을 사용하여 파일 로딩 동안 클라이언트 요청을 대기시킴
                if os.path.isfile(PINGLIST_FILE):
                    with open(PINGLIST_FILE, "r") as file:
                        # YAML 파일을 딕셔너리로 파싱
                        data_in_memory = yaml.safe_load(file)
                    print("Pinglist has been loaded into memory.")
                else:
                    print(f"Pinglist file not found at {PINGLIST_FILE}")
        except Exception as e:
            print(f"Error loading pinglist: {e}")

        # 60초마다 파일을 다시 로드
        await asyncio.sleep(60)


# 클라이언트의 pinglist 요청을 처리하는 핸들러
async def handle_pinglist_request(request):
    # 데이터를 읽는 동안에도 락을 걸어 데이터 일관성 보장
    async with data_lock:
        if data_in_memory:
            return web.json_response(
                data_in_memory
            )  # 메모리에 있는 pinglist 데이터를 JSON으로 응답
        else:
            return web.Response(status=503, text="Pinglist data is not available")


# 서버를 시작하고 비동기적으로 파일 로드 및 클라이언트 요청을 처리하는 함수
async def start_server():
    # aiohttp 애플리케이션 생성
    app = web.Application()

    # 경로 설정: /pinglist 엔드포인트로 클라이언트의 요청을 처리
    app.router.add_get("/pinglist", handle_pinglist_request)

    # 파일 로딩을 주기적으로 실행하는 백그라운드 작업
    asyncio.ensure_future(
        load_pinglist_periodically()
    )  # Python 3.6 호환성을 위해 asyncio.ensure_future 사용

    # 웹 서버 실행
    return app


if __name__ == "__main__":
    # aiohttp 서버 실행
    web.run_app(start_server(), port=PINGLIST_PORT)  # PINGLIST_PORT 사용
