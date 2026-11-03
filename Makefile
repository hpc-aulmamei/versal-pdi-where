CXX      ?= g++
CXXFLAGS ?= -O2 -g -Wall -Wextra -std=c++17
SRCS     := src/main.cpp src/pdi.cpp src/floorplan.cpp
HDRS     := src/pdi.hpp src/floorplan.hpp

pdi_where: $(SRCS) $(HDRS)
	$(CXX) $(CXXFLAGS) -o $@ $(SRCS) -lz

check: pdi_where
	./pdi_where --selftest

clean:
	rm -f pdi_where

.PHONY: check clean
