MAIN_SOURCES = $(shell grep -lE 'int main(.*) {' *.cc)
LIB_SOURCES  = $(shell grep -LE 'int main(.*) {' *.cc)
HEADERS  = $(wildcard *.h)
MAIN_OBJECTS  = $(addprefix _obj/, $(MAIN_SOURCES:.cc=.o))
LIB_OBJECTS  = $(addprefix _obj/, $(LIB_SOURCES:.cc=.o))
BINARIES = $(addprefix bin/, $(MAIN_SOURCES:.cc=))

LDFLAGS  = -L/usr/local/lib/ -lbfd -ldl -lz -liberty -lintl.8
CXXFLAGS = -std=c++11 -Wall -g -O2 
CXX = clang++

all: $(BINARIES)

.PHONY: clean
.SECONDARY: $(LIB_OBJECTS) $(MAIN_OBJECTS)

clean:
	rm -f *.o *.d $(LIB_OBJECTS) $(MAIN_OBJECTS) $(BINARIES)

bin/%: _obj/%.o $(LIB_OBJECTS) | bin
	$(CXX) -o $@ $^ $(LDFLAGS)

_obj/%.o: %.cc $(HEADERS) | _obj
	$(CXX) -o $@ -c $(CXXFLAGS) $<

_obj:
	mkdir -p _obj

bin:
	mkdir -p bin/
