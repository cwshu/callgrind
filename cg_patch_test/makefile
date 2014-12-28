CC = gcc
CXX = g++
CFLAGS = -g
CXXAGS = -g
JAVAC = javac

C_SRC = $(wildcard *.c)
CPP_SRC = $(wildcard *.cpp)
JAVA_SRC = $(wildcard *.java)

C_TESTCODE = $(patsubst %.c,%,$(C_SRC))
CPP_TESTCODE = $(patsubst %.cpp,%,$(CPP_SRC))
JAVA_TESTCODE = $(patsubst %.java,%.class,$(JAVA_SRC))

all: $(C_TESTCODE) $(CPP_TESTCODE) $(JAVA_TESTCODE)

clean: 
	rm -f $(C_TESTCODE) $(CPP_TESTCODE) $(JAVA_TESTCODE)

$(C_TESTCODE): %: %.c
	${CC} ${CFLAGS} $< -o $@

$(CPP_TESTCODE): %: %.cpp
	${CXX} ${CXXFLAGS} $< -o $@

$(JAVA_TESTCODE): %.class: %.java
	${JAVAC} $<

.PNOHY: all clean

