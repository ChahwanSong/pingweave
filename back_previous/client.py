import requests

# 포트 번호 매크로 선언
PINGLIST_PORT = 33335


# pinglist 데이터를 요청하는 클라이언트 코드
def fetch_pinglist(url):
    try:
        response = requests.get(url)
        if response.status_code == 200:
            pinglist_data = response.json()
            print("Pinglist received successfully.")
            print(pinglist_data)  # 받은 pinglist 데이터를 출력 또는 처리
        else:
            print(f"Failed to fetch pinglist - {response.status_code}")
    except Exception as e:
        print(f"Error occurred - {e}")


if __name__ == "__main__":
    url = "http://localhost:{}/pinglist".format(
        PINGLIST_PORT
    )  # Python 3.6 호환성을 위해 format 사용

    # 서버에서 pinglist 데이터를 요청
    fetch_pinglist(url)
