NAME = minishell
# Source files
SRCS = $(shell find src -type f -name "*.c")
OBJS = $(SRCS:.c=.o)
# Compiler and flags
CC =  c++
CFLAGS = -Wall -Wextra -Werror  -std=98 -g
# Include path for readline
INCLUDES = -Iinclude
# Linker flags


# Rules
all: $(NAME)
	

$(NAME): $(OBJS)

	$(CC) $(CFLAGS) $(OBJS)  -o $(NAME)

%.o: %.c
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

clean:
	rm -f $(OBJS)

fclean: clean
	rm -f $(NAME)

re: fclean all
# 🆕 Format rule, first run those: 

# valgrind : 
# 	valgrind --tool=memcheck --leak-check=full --show-leak-kinds=all --trace-children=yes --log-file=vg-logs/valgrind-%p.log --suppressions=readline.supp ./minishell

valgrind:
	@mkdir -p vg-logs
	@rm -rf vg-logs/*
	valgrind \
	--tool=memcheck \
	--leak-check=full \
	--show-leak-kinds=all \
	--trace-children=yes \
	--child-silent-after-fork=no \
	--log-file=vg-logs/valgrind-%p.log \
	./$(NAME)


.PHONY: all clean fclean re  valgrind