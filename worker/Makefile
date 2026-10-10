CXX = c++
CXXFLAGS = -std=c++17 -Wall -Wextra -Wpedantic -O2
JSON_FLAGS := $(shell pkg-config --cflags --libs json-c)

.PHONY: all clean test
all: worker
worker: worker.cpp
	$(CXX) $(CXXFLAGS) worker.cpp $(JSON_FLAGS) -o $@
test: worker
	python3 -m unittest discover -s . -p 'test_*.py'
clean:
	rm -f worker
