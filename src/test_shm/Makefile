# Compiler and flags
CXX = g++
CXXFLAGS = -std=c++17 -O2 -Wall -Wextra -I ../../libs -lpthread

# Target binary
TARGET = server

# Source file
SRC = server.cpp

# Default build target
all: $(TARGET)

# Single-step compilation and linking
$(TARGET):
	$(CXX) $(CXXFLAGS) $(SRC) -o $(TARGET)

clean:
	rm -f $(TARGET)