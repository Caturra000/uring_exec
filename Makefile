.PHONY: all clean

INCLUDE = include
EXAMPLES = examples
BENCH = bench
BUILD = build

ALL_EXAMPLES_TARGETS = $(notdir $(basename $(wildcard $(EXAMPLES)/*.cpp)))
ALL_BENCH_TARGETS = $(notdir $(basename $(wildcard $(BENCH)/*.cpp)))

CXX_FLAGS = -std=c++20 -Wall -Wextra -g -I$(INCLUDE) $^ -luring -pthread
CXX_FLAGS_DEBUG =

all: examples bench

examples: $(ALL_EXAMPLES_TARGETS)

bench: $(ALL_BENCH_TARGETS)

clean:
	@rm -rf $(BUILD)

%: $(EXAMPLES)/%.cpp
	@mkdir -p $(BUILD)
	$(CXX) $(CXX_FLAGS) $(CXX_FLAGS_DEBUG) -o $(BUILD)/$@

%: $(BENCH)/%.cpp
	@mkdir -p $(BUILD)
	$(CXX) $(CXX_FLAGS) $(CXX_FLAGS_DEBUG) -o $(BUILD)/$@
