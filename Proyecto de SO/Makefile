CXX = g++
CXXFLAGS = -std=c++17 -Wall -g
LDFLAGS = -lncurses -lpthread
TARGET = gestor_so
SRCS = codigo.cpp
OBJS = $(SRCS:.cpp=.o)

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)

run: $(TARGET)
	./$(TARGET)

valgrind: $(TARGET)
        valgrind --leak-check=full --show-leak-kinds=all ./$(TARGET)

.PHONY: clean run valgrind
