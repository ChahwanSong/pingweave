SOURCE_DIR := $(shell pwd)

# 컴파일러 설정
CXX = g++ 

# 공통 컴파일러 플래그
CXXFLAGS = -std=c++17 -I ../libs -O2 -pthread -DSOURCE_DIR=\"$(SOURCE_DIR)\"

# 공통 링크 플래그
LDFLAGS = -l ibverbs

# 공통 라이브러리
LDLIBS =

# 테스트용 추가 컴파일러 플래그
TEST_CXXFLAGS = 

# 테스트용 추가 링크 라이브러리
TEST_LDLIBS = 

# 메인용 추가 컴파일러 플래그
MAIN_CXXFLAGS = 

# 메인용 추가 링크 라이브러리
MAIN_LDLIBS = 

# 출력 파일 디렉토리
BIN_DIR = ../bin

# 공통 소스 파일
SRCS_COMMON = logger.cpp rdma_common.cpp producer_queue.cpp
OBJS_COMMON = $(addprefix $(BIN_DIR)/, $(SRCS_COMMON:.cpp=.o))

# 테스트용 소스 파일 및 오브젝트 파일
SRCS_TEST_PRODUCER_QUEUE = test_producer_queue.cpp $(SRCS_COMMON)
OBJS_TEST_PRODUCER_QUEUE = $(addprefix $(BIN_DIR)/, $(SRCS_TEST_PRODUCER_QUEUE:.cpp=.o))

SRCS_TEST_LOGGER = test_logger_main.cpp
OBJS_TEST_LOGGER = $(addprefix $(BIN_DIR)/, $(SRCS_TEST_PRODUCER_QUEUE:.cpp=.o))

# 메인용 소스 파일 및 오브젝트 파일
SRCS_MAIN = rdma_main.cpp $(SRCS_COMMON)
OBJS_MAIN = $(addprefix $(BIN_DIR)/, $(SRCS_MAIN:.cpp=.o))

# 출력 파일
TARGET_TEST_PRODUCER_QUEUE = $(BIN_DIR)/test_producer_queue
TARGET_TEST_LOGGER = $(BIN_DIR)/test_logger_main
TARGET_MAIN = $(BIN_DIR)/main

# 기본 타겟
.PHONY: all test main clean

all: main

# 테스트 빌드
test: $(TARGET_TEST_PRODUCER_QUEUE) $(TARGET_TEST_LOGGER)

$(TARGET_TEST_PRODUCER_QUEUE): $(OBJS_TEST_PRODUCER_QUEUE)
	@mkdir -p $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $(TEST_CXXFLAGS) $^ -o $@ $(LDFLAGS) $(TEST_LDLIBS)

$(TARGET_TEST_LOGGER): $(OBJS_TEST_LOGGER)
	@mkdir -p $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $(TEST_CXXFLAGS) $^ -o $@ $(LDFLAGS) $(TEST_LDLIBS)

# 메인 빌드
main: $(TARGET_MAIN)

$(TARGET_MAIN): $(OBJS_MAIN)
	@mkdir -p $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $(MAIN_CXXFLAGS) $^ -o $@ $(LDFLAGS) $(MAIN_LDLIBS)

# 오브젝트 파일 생성 규칙
$(BIN_DIR)/%.o: %.cpp
	@mkdir -p $(BIN_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

# clean 명령어로 빌드 파일들을 삭제
clean:
	rm -f $(OBJS_COMMON) $(OBJS_TEST) $(OBJS_MAIN)
	rm -f $(TARGET_TEST_PRODUCER_QUEUE) $(TARGET_MAIN)

# $@: 현재 타겟의 이름
# $^: 모든 의존성의 리스트
# $<: 첫 번째 의존성
