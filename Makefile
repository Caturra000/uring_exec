.PHONY: all clean

INCLUDE = include
EXAMPLES = examples
BUILD = build
ALL_TARGET_NAME = $(notdir $(basename $(wildcard $(EXAMPLES)/*.cpp)))

CXX_FLAGS = -std=c++20 -Wall -Wextra -g -I$(INCLUDE) $^ -luring -pthread
CXX_FLAGS_DEBUG =

all: $(ALL_TARGET_NAME)

clean:
	@rm -r $(BUILD)

%: $(EXAMPLES)/%.cpp
	@mkdir -p $(BUILD)
	-$(CXX) $(CXX_FLAGS) $(CXX_FLAGS_DEBUG) -o $(BUILD)/$@
