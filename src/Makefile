SOURCE_DIR := $(shell pwd)

# 컴파일러 설정
CXX = g++ 

# GCC 버전 체크
GCC_VERSION := $(shell $(CXX) -dumpversion | cut -f1 -d.)

# 공통 컴파일러 플래그
CXXFLAGS = -std=c++17 -I ../libs -I /usr/include/yaml-cpp -O2 -pthread -DSOURCE_DIR=\"$(SOURCE_DIR)\"

# # GCC 8인 경우 -lstdc++fs 추가
# ifeq ($(shell expr $(GCC_VERSION) \< 8), 1)  # GCC 7 이하
#     # CXXFLAGS += -D_GLIBCXX_USE_CXX11_ABI=0
#     # LDFLAGS = -lstdc++fs -l ibverbs
# 	$(error "GCC version $(GCC_VERSION) detected. GCC 8 or higher is required.")
# else ifeq ($(GCC_VERSION), 8)  # GCC 8
#     LDFLAGS = -lstdc++fs -l ibverbs
# else
#     LDFLAGS = -l ibverbs
# endif

LDFLAGS = -l ibverbs

# 공통 라이브러리
LDLIBS =

# 출력 파일 디렉토리
BIN_DIR = ../bin
RESULT_DIR = ../result
LOGS_DIR = ../logs
UPLOAD_DIR = ../upload
DOWNLOAD_DIR = ../download

# 공통 소스 파일
SRCS_COMMON = rdma_common.cpp
OBJS_COMMON = $(addprefix $(BIN_DIR)/, $(SRCS_COMMON:.cpp=.o))

# 메인용 소스 파일 및 오브젝트 파일
SRCS_MAIN = pingweave.cpp $(SRCS_COMMON)
OBJS_MAIN = $(addprefix $(BIN_DIR)/, $(SRCS_MAIN:.cpp=.o))

# 출력 파일
TARGET_MAIN = $(BIN_DIR)/pingweave

# 기본 타겟
.PHONY: all main clean

all: main

# 메인 빌드
main: $(TARGET_MAIN)

$(TARGET_MAIN): $(OBJS_MAIN)
	@mkdir -p $(BIN_DIR)
	@mkdir -p $(RESULT_DIR)
	@mkdir -p $(LOGS_DIR)
	@mkdir -p $(UPLOAD_DIR)
	@mkdir -p $(DOWNLOAD_DIR)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

# 오브젝트 파일 생성 규칙
$(BIN_DIR)/%.o: %.cpp
	@mkdir -p $(BIN_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

# clean 명령어로 빌드 파일들을 삭제
clean:
	rm -f $(OBJS_COMMON) $(OBJS_MAIN) $(TARGET_MAIN)
	
# $@: 현재 타겟의 이름
# $^: 모든 의존성의 리스트
# $<: 첫 번째 의존성
