SOURCE_DIR := $(shell pwd)

# 컴파일러 설정
CXX = g++ 

# GCC 버전 체크
GCC_VERSION := $(shell $(CXX) -dumpversion | cut -f1 -d.)

# 공통 컴파일러 플래그
CXXFLAGS = -std=c++17 -O2 -I ../libs -I /usr/include/yaml-cpp -DSOURCE_DIR=\"$(SOURCE_DIR)\"
LDFLAGS = -lpthread -lrt
RDMAFLAGS = -libverbs

# -----------------------------------------------------------
# 출력 파일 디렉토리
BIN_DIR = ../bin
RESULT_DIR = ../result
LOGS_DIR = ../logs
UPLOAD_DIR = ../upload
DOWNLOAD_DIR = ../download
HTML_DIR = ../html
IMAGE_DIR = ../image
SUMMARY_DIR = ../summary

# -----------------------------------------------------------
# 공통 소스 및 오브젝트 
SRCS_COMMON = common.cpp
OBJS_COMMON = $(addprefix $(BIN_DIR)/, $(SRCS_COMMON:.cpp=.o))

# TCPUDP 소스 및 오브젝트 
SRCS_TCPUDP = tcpudp_common.cpp 
OBJS_TCPUDP = $(addprefix $(BIN_DIR)/, $(SRCS_TCPUDP:.cpp=.o))

# RDMA 소스 및 오브젝트 
SRCS_RDMA = rdma_common.cpp 
OBJS_RDMA = $(addprefix $(BIN_DIR)/, $(SRCS_RDMA:.cpp=.o))

# -----------------------------------------------------------
# TCP_SERVER
SRCS_TCP_SERVER = tcp_server.cpp $(SRCS_COMMON) $(SRCS_TCPUDP)
OBJS_TCP_SERVER = $(addprefix $(BIN_DIR)/, $(SRCS_TCP_SERVER:.cpp=.o))
TARGET_TCP_SERVER = $(BIN_DIR)/pingweave_tcp_server

# TCP_CLIENT
SRCS_TCP_CLIENT = tcp_client.cpp $(SRCS_COMMON) $(SRCS_TCPUDP)
OBJS_TCP_CLIENT = $(addprefix $(BIN_DIR)/, $(SRCS_TCP_CLIENT:.cpp=.o))
TARGET_TCP_CLIENT = $(BIN_DIR)/pingweave_tcp_client

# -----------------------------------------------------------
# UDP_SERVER
SRCS_UDP_SERVER = udp_server.cpp $(SRCS_COMMON) $(SRCS_TCPUDP)
OBJS_UDP_SERVER = $(addprefix $(BIN_DIR)/, $(SRCS_UDP_SERVER:.cpp=.o))
TARGET_UDP_SERVER = $(BIN_DIR)/pingweave_udp_server

# UDP_CLIENT
SRCS_UDP_CLIENT = udp_client.cpp $(SRCS_COMMON) $(SRCS_TCPUDP)
OBJS_UDP_CLIENT = $(addprefix $(BIN_DIR)/, $(SRCS_UDP_CLIENT:.cpp=.o))
TARGET_UDP_CLIENT = $(BIN_DIR)/pingweave_udp_client

# -----------------------------------------------------------
# RDMA_SERVER
SRCS_RDMA_SERVER = rdma_server.cpp $(SRCS_COMMON) $(SRCS_RDMA)
OBJS_RDMA_SERVER = $(addprefix $(BIN_DIR)/, $(SRCS_RDMA_SERVER:.cpp=.o))
TARGET_RDMA_SERVER = $(BIN_DIR)/pingweave_rdma_server

# RDMA_CLIENT
SRCS_RDMA_CLIENT = rdma_client.cpp $(SRCS_COMMON) $(SRCS_RDMA)
OBJS_RDMA_CLIENT = $(addprefix $(BIN_DIR)/, $(SRCS_RDMA_CLIENT:.cpp=.o))
TARGET_RDMA_CLIENT = $(BIN_DIR)/pingweave_rdma_client

# -----------------------------------------------------------
# 기본 타겟
.PHONY: all mkdir tcp_client tcp_server udp_client udp_server rdma_client rdma_server clean

all: mkdir tcp_client tcp_server udp_client udp_server rdma_client rdma_server

# -----------------------------------------------------------
# 빌드
mkdir: 
	@mkdir -p $(BIN_DIR)
	@mkdir -p $(RESULT_DIR)
	@mkdir -p $(LOGS_DIR)
	@mkdir -p $(UPLOAD_DIR)
	@mkdir -p $(DOWNLOAD_DIR)
	@mkdir -p $(HTML_DIR)
	@mkdir -p $(IMAGE_DIR)
	@mkdir -p $(SUMMARY_DIR)

# tcp_server
tcp_server: $(TARGET_TCP_SERVER)
$(TARGET_TCP_SERVER): $(OBJS_TCP_SERVER)
	@mkdir -p $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

# tcp_client
tcp_client: $(TARGET_TCP_CLIENT)
$(TARGET_TCP_CLIENT): $(OBJS_TCP_CLIENT)
	@mkdir -p $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

# udp_server
udp_server: $(TARGET_UDP_SERVER)
$(TARGET_UDP_SERVER): $(OBJS_UDP_SERVER)
	@mkdir -p $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

# udp_client
udp_client: $(TARGET_UDP_CLIENT)
$(TARGET_UDP_CLIENT): $(OBJS_UDP_CLIENT)
	@mkdir -p $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

# rdma_server
rdma_server: $(TARGET_RDMA_SERVER)
$(TARGET_RDMA_SERVER): $(OBJS_RDMA_SERVER)
	@mkdir -p $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS) $(RDMAFLAGS)

# rdma_client
rdma_client: $(TARGET_RDMA_CLIENT)
$(TARGET_RDMA_CLIENT): $(OBJS_RDMA_CLIENT)
	@mkdir -p $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS) $(RDMAFLAGS)


# -----------------------------------------------------------
# 오브젝트 파일 생성 규칙
$(BIN_DIR)/%.o: %.cpp
	@mkdir -p $(BIN_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

# clean 명령어로 빌드 파일들을 삭제
clean:
	rm -f \
		$(OBJS_COMMON) $(OBJS_TCPUDP) $(OBJS_RDMA) \
		$(OBJS_TCP_SERVER) $(OBJS_TCP_CLIENT) \
		$(OBJS_UDP_SERVER) $(OBJS_UDP_CLIENT) \
		$(OBJS_RDMA_SERVER) $(OBJS_RDMA_CLIENT) \
		$(TARGET_TCP_SERVER) $(TARGET_TCP_CLIENT) \
		$(TARGET_UDP_SERVER) $(TARGET_UDP_CLIENT) \
		$(TARGET_RDMA_SERVER) $(TARGET_RDMA_CLIENT)
	rm -rf $(BIN_DIR) $(RESULT_DIR) \
			$(LOGS_DIR) $(UPLOAD_DIR) \
			$(DOWNLOAD_DIR) $(HTML_DIR) \
			$(IMAGE_DIR) $(SUMMARY_DIR)
# $@: 현재 타겟의 이름
# $^: 모든 의존성의 리스트
# $<: 첫 번째 의존성
