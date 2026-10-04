CXX = g++
CXXFLAGS = -std=c++17 -g -Wall -Wextra -pthread

all: proxy

proxy: proxy_server_with_cache.o proxy_parse.o
	$(CXX) $(CXXFLAGS) -o $@ $^

%.o: %.cpp proxy_parse.hpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -f proxy *.o

tar:
	tar -cvzf ass1.tgz proxy_server_with_cache.cpp README.md makefile proxy_parse.cpp proxy_parse.hpp

.PHONY: all clean tar
