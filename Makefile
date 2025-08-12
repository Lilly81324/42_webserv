# **************************************************************************** #
#                                    CONFIG                                    #
# **************************************************************************** #

NAME                := webserv
TEST_BIN            := test_webserv
CATCH2_LIB          := libcatch2.a

# ===== EVALUATION MODE (set EVAL=1 to disable testing for 42 evaluation) ===== #
ifeq ($(EVAL),1)
    DISABLE_TESTS := 1
endif

# Directories
SRC_DIR             := src
TEST_DIR            := tests
INC_DIR             := include
TESTS_INC_DIR       := $(TEST_DIR)/includes
CATCH2_DIR          := libraries/Catch2/src

BUILD_DIR           := build
OBJ98_DIR           := $(BUILD_DIR)/obj98
OBJ14_DIR           := $(BUILD_DIR)/obj14_tests
LIB_DIR             := $(BUILD_DIR)/lib

# Compilers & Flags
CXX                 := c++
CC                  := cc
RM                  := rm -rf
AR                  := ar rcs

# Optional parallelism ONLY at top level (avoid jobserver warnings in sub-makes)
ifeq ($(MAKELEVEL),0)
  J ?= $(shell nproc)         # set default parallelism; override with: make J=8
  MAKEFLAGS += -j$(J)
endif

CXXFLAGS_98         := -Wall -Wextra -Werror -std=c++98 -MMD -MP
CXXFLAGS_14         := -Wall -Wextra -Werror -std=c++14 -MMD -MP
CFLAGS_C            := -Wall -Wextra -Werror -std=c99   -MMD -MP

INCS                := -I$(INC_DIR)
TEST_INCS           := $(INCS) -I$(TESTS_INC_DIR) -I$(CATCH2_DIR)

# **************************************************************************** #
#                                   SOURCES                                    #
# **************************************************************************** #

# Production sources (auto-discovered)
SRCS_CPP            := $(shell find $(SRC_DIR) -type f -name "*.cpp")
SRCS_C              := $(shell find $(SRC_DIR) -type f -name "*.c")

# If you have a main.cpp, exclude it from the test link to avoid dual mains
MAIN_SRC            := $(SRC_DIR)/main.cpp
SRCS_CPP_NOMAIN     := $(filter-out $(MAIN_SRC),$(SRCS_CPP))

# Test sources (auto-discovered)
ifndef DISABLE_TESTS
    TEST_SRCS_CPP   := $(shell find $(TEST_DIR) -type f -name "*.cpp")
    # Catch2 sources (auto-discovered) - exclude tests and examples
    CATCH2_SRCS     := $(shell find $(CATCH2_DIR)/catch2 -type f -name "*.cpp" -not -path "*/tests/*" -not -path "*/examples/*")
else
    TEST_SRCS_CPP   :=
    CATCH2_SRCS     :=
endif

# Objects
OBJS_98             := $(SRCS_CPP:$(SRC_DIR)/%.cpp=$(OBJ98_DIR)/%.o) \
                       $(SRCS_C:$(SRC_DIR)/%.c=$(OBJ98_DIR)/%.o)
OBJS_98_NOMAIN      := $(SRCS_CPP_NOMAIN:$(SRC_DIR)/%.cpp=$(OBJ98_DIR)/%.o) \
                       $(SRCS_C:$(SRC_DIR)/%.c=$(OBJ98_DIR)/%.o)

TEST_OBJS_14        := $(TEST_SRCS_CPP:$(TEST_DIR)/%.cpp=$(OBJ14_DIR)/%.o)
CATCH2_OBJS_14      := $(CATCH2_SRCS:$(CATCH2_DIR)/%.cpp=$(OBJ14_DIR)/catch2/%.o)
CATCH2_LIB_PATH     := $(LIB_DIR)/$(CATCH2_LIB)

DEPS_98             := $(OBJS_98:.o=.d)
ifndef DISABLE_TESTS
    TEST_DEPS_14    := $(TEST_OBJS_14:.o=.d)
    CATCH2_DEPS_14  := $(CATCH2_OBJS_14:.o=.d)
else
    TEST_DEPS_14    :=
    CATCH2_DEPS_14  :=
endif

# **************************************************************************** #
#                                    RULES                                     #
# **************************************************************************** #

.PHONY: all clean fclean re test catch2-lib test-fast prod run-tests stats clean-tests clean-catch2 re-tests eval-info help
# prevent parallel execution for these targets
.NOTPARALLEL: clean fclean re

# ============================= 42 SCHOOL MANDATORY RULES ============================= #

all: $(NAME)

clean:
	@$(RM) $(BUILD_DIR)

fclean: clean
	@$(RM) $(NAME) $(TEST_BIN)

# run fclean then all sequentially (no race, no warnings)
re:
	@$(MAKE) -s fclean
	@$(MAKE) -s all

# ============================ ADDITIONAL PROJECT RULES ============================ #

prod: $(NAME)

ifndef DISABLE_TESTS
test: $(TEST_BIN)

test-fast: $(TEST_BIN)
	@echo "Running tests..."
	@./$(TEST_BIN)
else
test:
	@echo "Tests disabled in evaluation mode. Use 'make EVAL=0 test' to enable."
test-fast:
	@echo "Tests disabled in evaluation mode. Use 'make EVAL=0 test-fast' to enable."
endif

eval-info:
	@echo "=== Evaluation Mode ==="
	@echo "To disable tests for 42 evaluation: make EVAL=1"
	@echo "To enable tests for development: make EVAL=0 (or just 'make test')"
	@echo "Current mode: $(if $(DISABLE_TESTS),EVALUATION (tests disabled),DEVELOPMENT (tests enabled))"

help:
	@echo "=== Available Make Targets ==="
	@echo "make / make all  - Build webserv binary (C++98)"
	@echo "make clean       - Remove object files"
	@echo "make fclean      - Remove objects and binaries"
	@echo "make re          - Clean and rebuild (sequential)"
	@echo "make test        - Build tests (if enabled)"
	@echo "make test-fast   - Build + run tests"
	@echo "make run-tests   - Run tests without rebuilding"
	@echo "make catch2-lib  - Build Catch2 static library"
	@echo "make stats       - Show compilation statistics"
	@echo "EVAL=1           - Disable tests for 42 evaluation"

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

# ------------------------------ Test executable ----------------------------- #

ifndef DISABLE_TESTS
$(CATCH2_LIB_PATH): $(CATCH2_OBJS_14)
	@mkdir -p $(LIB_DIR)
	@echo "Creating Catch2 library..."
	@$(AR) $@ $^

$(TEST_BIN): $(OBJS_98_NOMAIN) $(TEST_OBJS_14) $(CATCH2_LIB_PATH)
	@echo "Linking test executable..."
	@$(CXX) $(OBJS_98_NOMAIN) $(TEST_OBJS_14) $(CATCH2_LIB_PATH) -o $@

$(OBJ14_DIR)/%.o: $(TEST_DIR)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS_14) $(TEST_INCS) -c $< -o $@

$(OBJ14_DIR)/catch2/%.o: $(CATCH2_DIR)/%.cpp
	@mkdir -p $(dir $@)
	@echo "Compiling Catch2: $(notdir $<)"
	@$(CXX) $(CXXFLAGS_14) $(TEST_INCS) -c $< -o $@
endif

# ------------------------------ Utility targets ------------------------------ #

run-tests:
	@if [ -f $(TEST_BIN) ]; then \
		echo "Running tests..."; \
		./$(TEST_BIN); \
	else \
		echo "Test binary not found. Run 'make test' first."; \
		exit 1; \
	fi

stats:
	@echo "=== Project Statistics ==="
	@echo "Production sources: $(words $(SRCS_CPP)) C++ files, $(words $(SRCS_C)) C files"
	@echo "Test sources: $(words $(TEST_SRCS_CPP)) files"
	@echo "Catch2 sources: $(words $(CATCH2_SRCS)) files"
	@echo "Total object files to build: $(words $(OBJS_98) $(TEST_OBJS_14) $(CATCH2_OBJS_14))"

clean-tests:
	@$(RM) $(OBJ14_DIR) $(LIB_DIR) $(TEST_BIN)

clean-catch2:
	@$(RM) $(OBJ14_DIR)/catch2 $(CATCH2_LIB_PATH)

re-tests: clean-tests test

# ------------------------------ Dependencies -------------------------------- #

-include $(DEPS_98)
-include $(TEST_DEPS_14)
-include $(CATCH2_DEPS_14)

