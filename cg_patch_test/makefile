CC = gcc
CXX = g++
CFLAGS = -g
CXXAGS = -g

C_SRC = $(wildcard *.c)
CPP_SRC = $(wildcard *.cpp)

C_TESTCODE = $(patsubst %.c,%,$(C_SRC))
CPP_TESTCODE = $(patsubst %.cpp,%,$(CPP_SRC))

all: $(C_TESTCODE) $(CPP_TESTCODE)

clean: 
	rm -f $(C_TESTCODE) $(CPP_TESTCODE)

$(C_TESTCODE): %: %.c
	${CC} ${CFLAGS} $< -o $@

$(CPP_TESTCODE): %: %.cpp
	${CXX} ${CXXFLAGS} $< -o $@

.PNOHY: all clean

