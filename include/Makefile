NAME = webserv

CXX = c++

CXXFLAGS = -Wall -Werror -Wextra -std=c++98 -pedantic -g

RM = rm -f

INC = -Iinclude

SRC = \
	src/main.cpp \
	src/Server.cpp \
	src/Config.cpp \
	src/Logger.cpp \
	src/ConfigPrinter.cpp

OBJS = $(SRC:.cpp=.o)

all: $(NAME)

$(NAME) : $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $(OBJS)

%.o : %.cpp
	$(CXX) $(CXXFLAGS) $(INC) -c $< -o $@

clean:
	$(RM) $(OBJS)

fclean: clean
	$(RM) $(NAME)

re: fclean all

.PHONY: all clean fclean re