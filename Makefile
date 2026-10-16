CXX      := clang++
CXXFLAGS := -std=c++23 -Wall -Wextra -Wpedantic -Wno-c2y-extensions -I.
PYTHON   := python3
BUILD    := build

HEADERS := dasmig/core.hpp $(wildcard dasmig/core/*.hpp)

EXAMPLE_SRC := examples/example.cpp
EXAMPLE_BIN := $(BUILD)/example

TEST_SRC := tests/tests.cpp tests/tests_data.cpp tests/tests_batch.cpp tests/catch_amalgamated.cpp
TEST_BIN := $(BUILD)/test_runner

COV_FLAGS := -fprofile-instr-generate -fcoverage-mapping
COV_DIR   := coverage

.PHONY: all clean lint test coverage docs kat embed-fixtures

all: $(EXAMPLE_BIN)

$(EXAMPLE_BIN): $(EXAMPLE_SRC) $(HEADERS) | $(BUILD)
	$(CXX) $(CXXFLAGS) -o $@ $(EXAMPLE_SRC)

$(TEST_BIN): $(TEST_SRC) tests/kat_vectors.hpp tests/embedded_fixture.hpp $(HEADERS) | $(BUILD)
	$(CXX) $(CXXFLAGS) -o $@ $(TEST_SRC)

test: $(TEST_BIN)
	./$(TEST_BIN)

coverage: $(TEST_SRC) $(HEADERS) | $(BUILD)
	$(CXX) $(CXXFLAGS) $(COV_FLAGS) -o $(BUILD)/test_cov $(TEST_SRC)
	cd $(BUILD) && ./test_cov
	llvm-profdata merge -sparse $(BUILD)/default.profraw -o $(BUILD)/tests.profdata
	llvm-cov report $(BUILD)/test_cov --instr-profile=$(BUILD)/tests.profdata --sources $(HEADERS)
	llvm-cov show $(BUILD)/test_cov --instr-profile=$(BUILD)/tests.profdata --sources $(HEADERS) --format=html -o $(COV_DIR)
	@echo "Coverage report: $(COV_DIR)/index.html"

# Regenerate the known-answer vectors from the Python reference implementation.
kat:
	$(PYTHON) scripts/reference.py tests/kat_vectors.hpp

# Regenerate the resource fixtures and their embedded header.
embed-fixtures:
	$(PYTHON) scripts/make_fixtures.py
	$(PYTHON) scripts/embed.py tests/fixtures/resources --tier lite --name fixture \
		--output tests/embedded_fixture.hpp --include ../dasmig/core.hpp --chunk-bytes 200

$(BUILD):
	@mkdir -p $(BUILD)

docs:
	doxygen Doxyfile
	@echo "API docs: doc/api/html/index.html"

lint:
	clang-tidy $(EXAMPLE_SRC) -- $(CXXFLAGS)

clean:
	rm -rf $(BUILD) $(COV_DIR) doc/api
