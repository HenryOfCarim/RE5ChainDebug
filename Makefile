# Cross build for the 32-bit ASI (tested with mingw-w64 and TDM-GCC).
# On Linux:   make CXX=i686-w64-mingw32-g++
# On Windows: mingw32-make CXX=g++

CXX      ?= i686-w64-mingw32-g++
CXXFLAGS ?= -m32 -shared -O2 -Wall -static-libgcc -static-libstdc++
TARGET   ?= re5_chaindbg.asi
SRC      := src/dllmain.cpp
HDR      := src/rest_links.h

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(SRC) $(HDR)
	$(CXX) $(CXXFLAGS) -o $@ $(SRC)

clean:
	rm -f $(TARGET)
