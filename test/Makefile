# Compiler
CXX = g++

# Flags
CXXFLAGS = -Wall -O2 -g
LDFLAGS = -libverbs -pthread

# Target executable
TARGET = rping

# Source files
SRCS = rping.cpp
DEPS = common.hpp  # 의존성 파일

# Default target
all: $(TARGET)

# Compile and link in one step to create the executable
$(TARGET): $(SRCS) $(DEPS)
	$(CXX) $(CXXFLAGS) -o $(TARGET) $(SRCS) $(LDFLAGS)

# Clean up the compiled files
clean:
	rm -f $(TARGET)

# Phony targets
.PHONY: all clean
