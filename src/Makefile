# 컴파일러 설정
CXX = g++
CXXFLAGS = -std=c++17 -I ../libs

# 소스 파일 목록
SRCS = test_producer_queue.cpp producer_queue.cpp common.cpp

# 출력 파일
TARGET = test_producer_queue

# 빌드 규칙
all: $(TARGET)

$(TARGET):
	$(CXX) $(CXXFLAGS) $(SRCS) -o $(TARGET)

# clean 명령어로 빌드 파일들을 삭제
clean:
	rm -f $(TARGET)
