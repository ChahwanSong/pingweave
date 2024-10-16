# 컴파일러 설정
CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -O2

# 외부 라이브러리 경로 설정
INCLUDES = -I/usr/include  # -I/usr/include/yaml-cpp -I/usr/include/nlohmann

# 링크할 라이브러리 설정
LIBS = -lboost_system -lpthread -lyaml-cpp -libverbs  # libverbs 추가

# 대상 파일 이름
SERVER_TARGET = server
CLIENT_TARGET = client

# 소스 파일
SERVER_SRCS = server.cpp
CLIENT_SRCS = client.cpp

# 객체 파일
SERVER_OBJS = $(SERVER_SRCS:.cpp=.o)
CLIENT_OBJS = $(CLIENT_SRCS:.cpp=.o)

# 기본 빌드 대상: 서버와 클라이언트 모두 빌드
all: $(SERVER_TARGET) $(CLIENT_TARGET)

# 서버 실행 파일 빌드 규칙
$(SERVER_TARGET): $(SERVER_OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LIBS)

# 클라이언트 실행 파일 빌드 규칙
$(CLIENT_TARGET): $(CLIENT_OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LIBS)

# 소스 파일을 객체 파일로 컴파일하는 규칙 (서버)
%.o: %.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

# 클린업
clean:
	rm -f $(SERVER_OBJS) $(CLIENT_OBJS) $(SERVER_TARGET) $(CLIENT_TARGET)
