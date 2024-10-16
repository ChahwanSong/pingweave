# Compiler and flags
CXX = g++
CXXFLAGS = -std=c++17 -Wall

# Target executable
TARGET = main

# Source files
SRCS = main.cpp

# Default rule
all: $(TARGET)

# Rule to create the executable
$(TARGET): $(SRCS)
	$(CXX) $(CXXFLAGS) -o $(TARGET) $(SRCS) -lrt

# Rule for cleaning up
clean:
	rm -f $(TARGET)
