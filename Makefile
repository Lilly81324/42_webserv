# **************************************************************************** #
#                                    CONFIG                                    #
# **************************************************************************** #

NAME                := webserv

# Test binaries
TEST_BIN            := test_webserv
TEST_BIN_STUBS      := test_webserv_stubs
CATCH2_LIB          := libcatch2.a

# ===== EVALUATION MODE (set EVAL=1 to disable testing for 42 evaluation) ===== #
ifeq ($(EVAL),1)
    DISABLE_TESTS := 1
endif

# Directories
SRC_DIR         := src
TEST_DIR        := tests
INC_DIR         := include
TESTS_INC_DIR   := $(TEST_DIR)/includes
CATCH2_DIR      := libraries/Catch2/src

BUILD_DIR       := build
OBJ98_DIR       := $(BUILD_DIR)/obj98
OBJ98_STUBS_DIR := $(BUILD_DIR)/obj98_stubs
OBJ14_DIR       := $(BUILD_DIR)/obj14_tests
LIB_DIR         := $(BUILD_DIR)/lib

# Compiler & Flags (42-style)
CXX             := c++
CC              := cc
RM              := rm -rf
AR              := ar rcs

# Enable parallel compilation by default
MAKEFLAGS       += -j$(shell nproc)

CXXFLAGS_98		:= -Wall -Wextra -Werror -std=c++98 -MMD -MP -g
CXXFLAGS_14		:= -std=c++14 -MMD -MP -g
CFLAGS_C		:= -Wall -Wextra -Werror -std=c99   -MMD -MP -g
TESTFLAGS		:= --reporter console --durations yes

# Includes
INCS            := -I$(INC_DIR)
# IMPORTANT: For tests, put stubs FIRST so they win when USE_STUBS is defined
TEST_INCS       := -I$(TESTS_INC_DIR) -I$(INC_DIR) -I$(CATCH2_DIR)

# **************************************************************************** #
#                                   SOURCES                                    #
# **************************************************************************** #

# Production sources (auto-discovered)
SRCS_CPP        := $(shell find $(SRC_DIR) -type f -name "*.cpp")
SRCS_C          := $(shell find $(SRC_DIR) -type f -name "*.c")

# If you have a main.cpp, exclude it from the test link to avoid dual mains
MAIN_SRC        := $(SRC_DIR)/main.cpp
SRCS_CPP_NOMAIN := $(filter-out $(MAIN_SRC),$(SRCS_CPP))

# Test sources (auto-discovered)
ifndef DISABLE_TESTS
    TEST_SRCS_CPP	:= $(shell find $(TEST_DIR) -type f -name "*.cpp")
    # Catch2 sources (auto-discovered) - exclude tests and examples
    CATCH2_SRCS		:= $(shell find $(CATCH2_DIR)/catch2 -type f -name "*.cpp" -not -path "*/tests/*" -not -path "*/examples/*")
else
    TEST_SRCS_CPP	:=
    CATCH2_SRCS		:=
endif

# Objects
OBJS_98             := $(SRCS_CPP:$(SRC_DIR)/%.cpp=$(OBJ98_DIR)/%.o) \
                       $(SRCS_C:$(SRC_DIR)/%.c=$(OBJ98_DIR)/%.o)

OBJS_98_NOMAIN      := $(SRCS_CPP_NOMAIN:$(SRC_DIR)/%.cpp=$(OBJ98_DIR)/%.o) \
                       $(SRCS_C:$(SRC_DIR)/%.c=$(OBJ98_DIR)/%.o)

# Stub-compiled (server built against stubs for tests)
OBJS_98_STUBS       := $(SRCS_CPP_NOMAIN:$(SRC_DIR)/%.cpp=$(OBJ98_STUBS_DIR)/%.o) \
                       $(SRCS_C:$(SRC_DIR)/%.c=$(OBJ98_STUBS_DIR)/%.o)

# Tests & Catch2
TEST_OBJS_14        := $(TEST_SRCS_CPP:$(TEST_DIR)/%.cpp=$(OBJ14_DIR)/%.o)
CATCH2_OBJS_14      := $(CATCH2_SRCS:$(CATCH2_DIR)/%.cpp=$(OBJ14_DIR)/catch2/%.o)
CATCH2_LIB_PATH     := $(LIB_DIR)/$(CATCH2_LIB)

# Deps
DEPS_98             := $(OBJS_98:.o=.d)
ifndef DISABLE_TESTS
    TEST_DEPS_14    := $(TEST_OBJS_14:.o=.d)
    CATCH2_DEPS_14  := $(CATCH2_OBJS_14:.o=.d)
    DEPS_98_STUBS   := $(OBJS_98_STUBS:.o=.d)
else
    TEST_DEPS_14    :=
    CATCH2_DEPS_14  :=
    DEPS_98_STUBS   :=
endif

# **************************************************************************** #
#                                    RULES                                     #
# **************************************************************************** #

.PHONY: all clean fclean re prod test test-fast test-stubs run-tests run-tests-stubs \
        catch2-lib clean-tests clean-catch2 re-tests eval-info help

# ============================= 42 SCHOOL MANDATORY RULES ============================= #

# 42 School Standard: 'make' must build the main program (C++98)
all: $(NAME)

# 42 School Standard: remove object files
clean:
	@$(RM) $(BUILD_DIR)

# 42 School Standard: remove object files and executable
fclean: clean
	@$(RM) $(NAME) $(TEST_BIN) $(TEST_BIN_STUBS)

# 42 School Standard: clean and rebuild everything
re: fclean all

# ============================ ADDITIONAL PROJECT RULES ============================ #

# Production app (same as 'all' for 42 compliance)
prod: $(NAME)

ifndef DISABLE_TESTS
# Build Catch2 as a static library (built once, reused many times)
catch2-lib: $(CATCH2_LIB_PATH)

# Standard tests (link against prod-compiled server objects)
test: $(TEST_BIN)

# Fast test target (builds and runs tests in one command)
test-fast: $(TEST_BIN)
	@echo "Running tests..."
	@./$(TEST_BIN) $(TESTFLAGS)
else
test:
	@echo "Tests disabled in evaluation mode. Use 'make EVAL=0 test' to enable."

test-fast:
	@echo "Tests disabled in evaluation mode. Use 'make EVAL=0 test-fast' to enable."

test-stubs:
	@echo "Tests disabled in evaluation mode. Use 'make EVAL=0 test-stubs' to enable."

run-tests:
	@echo "Tests disabled in evaluation mode. Use 'make EVAL=0 run-tests' to enable."

run-tests-stubs:
	@echo "Tests disabled in evaluation mode. Use 'make EVAL=0 run-tests-stubs' to enable."
endif

# ============================= EVALUATION MODE INFO ============================= #
eval-info:
	@echo "=== Evaluation Mode ==="
	@echo "To disable tests for 42 evaluation: make EVAL=1"
	@echo "To enable tests for development: make EVAL=0 (or just 'make test')"
	@echo "Current mode: $(if $(DISABLE_TESTS),EVALUATION (tests disabled),DEVELOPMENT (tests enabled))"

# ================================== HELP ==================================== #
help:
	@echo "=== Available Make Targets ==="
	@echo ""
	@echo "🎯 42 School Standard Targets:"
	@echo "  make / make all    - Build webserv binary (C++98)"
	@echo "  make clean         - Remove object files"
	@echo "  make fclean        - Remove object files and binaries"
	@echo "  make re            - Clean and rebuild everything"
	@echo ""
	@echo "🧪 Testing Targets:"
	@echo "  make catch2-lib    - Build Catch2 static library"
	@echo "  make test          - Build standard test executable (prod headers)"
	@echo "  make test-fast     - Build & run standard tests"
	@echo "  make test-stubs    - Build stubbed test executable (uses stubs)"
	@echo "  make run-tests     - Run standard tests without rebuilding"
	@echo "  make run-tests-stubs - Run stubbed tests without rebuilding"
	@echo ""
	@echo "🏗️  Utility Targets:"
	@echo "  make prod          - Build production binary (same as 'all')"
	@echo "  make stats         - Show compilation statistics"
	@echo "  make clean-tests   - Clean only test files"
	@echo "  make clean-catch2  - Force rebuild of Catch2"
	@echo "  make re-tests      - Quick rebuild of tests"
	@echo ""
	@echo "🎓 Evaluation Mode:"
	@echo "  make EVAL=1        - Disable tests for 42 evaluation"
	@echo "  make eval-info     - Show current evaluation mode"

# ------------------------------ Production app ------------------------------ #

$(NAME): $(OBJS_98)
	$(CXX) $(OBJS_98) -o $@

# C++98 objects (mirror subdirs)
$(OBJ98_DIR)/%.o: $(SRC_DIR)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS_98) $(INCS) -c $< -o $@

# C objects 
$(OBJ98_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS_C) $(INCS) -c $< -o $@

# ------------------------------ Test executable (standard) ----------------------------- #

ifndef DISABLE_TESTS
# Build Catch2 as a static library (built once, reused many times)
$(CATCH2_LIB_PATH): $(CATCH2_OBJS_14)
	@mkdir -p $(LIB_DIR)
	@echo "Creating Catch2 library..."
	@$(AR) $@ $^

# Standard tests link against production-compiled server objects
$(TEST_BIN): $(OBJS_98_NOMAIN) $(TEST_OBJS_14) $(CATCH2_LIB_PATH)
	@echo "Linking test executable..."
	$(CXX) $(OBJS_98_NOMAIN) $(TEST_OBJS_14) $(CATCH2_LIB_PATH) -o $@

# C++14 test objects (Catch2-enabled)
$(OBJ14_DIR)/%.o: $(TEST_DIR)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS_14) -DUNIT_TEST $(TEST_INCS) -c $< -o $@

# C++14 Catch2 objects (with progress indicator)
$(OBJ14_DIR)/catch2/%.o: $(CATCH2_DIR)/%.cpp
	@mkdir -p $(dir $@)
	@echo "Compiling Catch2: $(notdir $<)"
	@$(CXX) $(CXXFLAGS_14) $(TEST_INCS) -c $< -o $@
endif

# ------------------------------ Test executable (STUBS) ------------------------------ #

ifndef DISABLE_TESTS
# Recompile server/library objects for tests with stubs + test hooks
$(OBJ98_STUBS_DIR)/%.o: $(SRC_DIR)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS_98) -DUSE_STUBS -DUNIT_TEST $(TEST_INCS) -c $< -o $@

$(OBJ98_STUBS_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS_C) -DUSE_STUBS -DUNIT_TEST $(TEST_INCS) -c $< -o $@


# Stubbed test binary links against OBJS_98_STUBS
$(TEST_BIN_STUBS): $(OBJS_98_STUBS) $(TEST_OBJS_14) $(CATCH2_LIB_PATH)
	@echo "Linking stubbed test executable..."
	$(CXX) $(OBJS_98_STUBS) $(TEST_OBJS_14) $(CATCH2_LIB_PATH) -o $@

endif

# ------------------------------ Utility targets ------------------------------ #

# Run tests without rebuilding
run-tests:
	@if [ -f $(TEST_BIN) ]; then \
		echo "Running tests..."; \
		./$(TEST_BIN) $(TESTFLAGS) ; \
	else \
		echo "Test binary not found. Run 'make test' first."; \
		exit 1; \
	fi

# Show compilation statistics
stats:
	@echo "=== Project Statistics ==="
	@echo "Production sources: $(words $(SRCS_CPP)) C++ files, $(words $(SRCS_C)) C files"
	@echo "Test sources: $(words $(TEST_SRCS_CPP)) files"
	@echo "Catch2 sources: $(words $(CATCH2_SRCS)) files"
	@echo "Total object files to build: $(words $(OBJS_98) $(OBJS_98_STUBS) $(TEST_OBJS_14) $(CATCH2_OBJS_14))"

# Clean only test-related files (keep production objects)
clean-tests:
	@$(RM) $(OBJ14_DIR) $(LIB_DIR) $(TEST_BIN) $(TEST_BIN_STUBS) $(OBJ98_STUBS_DIR)

# Clean only Catch2 library (force rebuild of Catch2)
clean-catch2:
	@$(RM) $(OBJ14_DIR)/catch2 $(CATCH2_LIB_PATH)

# Quick rebuild of just tests (standard tests)
re-tests: clean-tests test

# ------------------------------ Dependencies -------------------------------- #

-include $(DEPS_98)
-include $(DEPS_98_STUBS)
-include $(TEST_DEPS_14)
-include $(CATCH2_DEPS_14)
