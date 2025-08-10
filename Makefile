# **************************************************************************** #
#                                    CONFIG                                    #
# **************************************************************************** #

NAME				:= webserv
TEST_BIN		:= test_webserv

# Directories
SRC_DIR			:= src
TEST_DIR		:= tests
INC_DIR			:= include
TESTS_INC_DIR	:= $(TEST_DIR)/includes
CATCH2_DIR		:= libraries/Catch2

BUILD_DIR		:= build
OBJ98_DIR		:= $(BUILD_DIR)/obj98
OBJ11_DIR		:= $(BUILD_DIR)/obj11_tests

# Compiler & Flags (42-style)
CXX				:= c++
CC				:= cc
RM				:= rm -rf

CXXFLAGS_98		:= -Wall -Wextra -Werror -std=c++98 -MMD -MP
CXXFLAGS_11		:= -Wall -Wextra -Werror -std=c++11 -MMD -MP
CFLAGS_C		:= -Wall -Wextra -Werror -std=c99   -MMD -MP

INCS			:= -I$(INC_DIR)
TEST_INCS		:= $(INCS) -I$(TESTS_INC_DIR) -I$(CATCH2_DIR)

# **************************************************************************** #
#                                   SOURCES                                    #
# **************************************************************************** #

# Production sources (auto-discovered)
SRCS_CPP		:= $(shell find $(SRC_DIR) -type f -name "*.cpp")
SRCS_C			:= $(shell find $(SRC_DIR) -type f -name "*.c")

# If you have a main.cpp, exclude it from the test link to avoid dual mains
MAIN_SRC		:= $(SRC_DIR)/main.cpp
SRCS_CPP_NOMAIN	:= $(filter-out $(MAIN_SRC),$(SRCS_CPP))

# Test sources (auto-discovered)
TEST_SRCS_CPP	:= $(shell find $(TEST_DIR) -type f -name "*.cpp")

# Objects
OBJS_98			:= $(SRCS_CPP:$(SRC_DIR)/%.cpp=$(OBJ98_DIR)/%.o) \
					$(SRCS_C:$(SRC_DIR)/%.c=$(OBJ98_DIR)/%.o)
OBJS_98_NOMAIN	:= $(SRCS_CPP_NOMAIN:$(SRC_DIR)/%.cpp=$(OBJ98_DIR)/%.o) \
					$(SRCS_C:$(SRC_DIR)/%.c=$(OBJ98_DIR)/%.o)

TEST_OBJS_11	:= $(TEST_SRCS_CPP:$(TEST_DIR)/%.cpp=$(OBJ11_DIR)/%.o)

DEPS_98			:= $(OBJS_98:.o=.d)
TEST_DEPS_11	:= $(TEST_OBJS_11:.o=.d)

# **************************************************************************** #
#                                    RULES                                     #
# **************************************************************************** #

.PHONY: all clean fclean re test

all: $(NAME)

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

test: $(TEST_BIN)

$(TEST_BIN): $(OBJS_98_NOMAIN) $(TEST_OBJS_11)
	$(CXX) $(OBJS_98_NOMAIN) $(TEST_OBJS_11) -o $@

# C++11 test objects (Catch2-enabled)
$(OBJ11_DIR)/%.o: $(TEST_DIR)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS_11) $(TEST_INCS) -c $< -o $@

# ------------------------------ Housekeeping -------------------------------- #

clean:
	@$(RM) $(BUILD_DIR)

fclean: clean
	@$(RM) $(NAME) $(TEST_BIN)

re: fclean all

# ------------------------------ Dependencies -------------------------------- #

-include $(DEPS_98)
-include $(TEST_DEPS_11)
