CXX = g++
CXXFLAGS = -std=c++17 -O2 -Wall -Wextra

SRC_DIR = src

all: collector reporter

collector: $(SRC_DIR)/collector.cpp
	$(CXX) $(CXXFLAGS) -o collector $(SRC_DIR)/collector.cpp

reporter: $(SRC_DIR)/reporter.cpp
	$(CXX) $(CXXFLAGS) -o reporter $(SRC_DIR)/reporter.cpp -lcurl

clean:
	rm -f collector reporter
