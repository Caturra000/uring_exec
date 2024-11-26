.PHONY: all clean

INCLUDE = include
EXAMPLES = examples
BENCH = benchmarks
BUILD = build

# Will not compile.
NEED_EXTERNAL_DEPENDENCY = $(BENCH)/pong_asio.cpp

# NEED_EXTERNAL_DEPENDENCY is not included.
ALL_EXAMPLES_TARGETS = $(notdir $(basename $(wildcard $(EXAMPLES)/*.cpp)))
ALL_BENCH_TARGETS = $(notdir $(basename $(filter-out $(NEED_EXTERNAL_DEPENDENCY), $(wildcard $(BENCH)/*.cpp))))

CXX_FLAGS = -std=c++20 -Wall -Wextra -g -I$(INCLUDE) $^ -luring -pthread
CXX_FLAGS_DEBUG =

all: examples benchmarks

examples: $(ALL_EXAMPLES_TARGETS)

benchmarks: $(ALL_BENCH_TARGETS)

benchmark_script:
	python $(BENCH)/pingpong.py

clean:
	@rm -rf $(BUILD)

%: $(EXAMPLES)/%.cpp
	@mkdir -p $(BUILD)
	$(CXX) $(CXX_FLAGS) $(CXX_FLAGS_DEBUG) -o $(BUILD)/$@

%: $(BENCH)/%.cpp
	@mkdir -p $(BUILD)
	$(CXX) $(CXX_FLAGS) $(CXX_FLAGS_DEBUG) -O3 -o $(BUILD)/$@
