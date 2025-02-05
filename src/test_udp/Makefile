# SOURCE_DIR을 ../ 로 설정
SOURCE_DIR := $(shell realpath ../)

CXX := g++
CXXFLAGS := -std=c++17 -O2 -pthread -I $(SOURCE_DIR)/../libs -I $(SOURCE_DIR) -I /usr/include/yaml-cpp -DSOURCE_DIR=\"$(SOURCE_DIR)\"
LDFLAGS := -libverbs -lpthread -lrt

# 공통 오브젝트 파일
COMMON_OBJ := $(SOURCE_DIR)/common.o $(SOURCE_DIR)/tcpudp_common.o

# 개별 오브젝트 파일
SERVER_OBJ := pw_server.o
CLIENT_OBJ := pw_client.o

# 실행 파일
TARGET_SERVER := server
TARGET_CLIENT := client

# 모든 실행 파일 빌드
all: $(TARGET_SERVER) $(TARGET_CLIENT)

# 서버 빌드
$(TARGET_SERVER): $(COMMON_OBJ) $(SERVER_OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

# 클라이언트 빌드
$(TARGET_CLIENT): $(COMMON_OBJ) $(CLIENT_OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

# 현재 디렉토리에 common.o, tcpudp_common.o 빌드
common.o: $(SOURCE_DIR)/common.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

tcpudp_common.o: $(SOURCE_DIR)/tcpudp_common.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

# 일반적인 .cpp → .o 빌드 규칙 (현재 디렉토리에 빌드)
%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

# 정리(clean)
clean:
	rm -f $(COMMON_OBJ) $(SERVER_OBJ) $(CLIENT_OBJ) $(TARGET_SERVER) $(TARGET_CLIENT)

.PHONY: all clean
