# Makefile for Natural Calculator (native Win32/GDI, no external deps).
#
# Build on Windows with MinGW-w64:
#   mingw32-make
#
# Cross-compile from Linux/macOS with MinGW-w64:
#   make CXX=x86_64-w64-mingw32-g++ WINDRES=x86_64-w64-mingw32-windres
#
# Produces build/NaturalCalculator.exe -- a single, dependency-free,
# statically-linked executable (no MSVCRT/libgcc/libstdc++ DLLs required
# at runtime) sized for near-instant startup.

CXX      ?= g++
WINDRES  ?= windres
SRC_DIR  := src
BUILD_DIR:= build
TARGET   := $(BUILD_DIR)/NaturalCalculator.exe
RC_OBJECT:= $(BUILD_DIR)/app_res.o

SOURCES  := $(wildcard $(SRC_DIR)/*.cpp)
OBJECTS  := $(patsubst $(SRC_DIR)/%.cpp,$(BUILD_DIR)/%.o,$(SOURCES))

CXXFLAGS := -std=c++17 -O2 -Wall -Wextra -municode -mwindows -DUNICODE -D_UNICODE
LDFLAGS  := -static -static-libgcc -static-libstdc++ -municode -mwindows -s
LIBS     := -lgdi32 -luser32 -lkernel32 -ldwmapi -ladvapi32 -lcomctl32 -lcomdlg32 -lgdiplus -lole32 -ld3d11 -ldxgi

.PHONY: all clean run test

all: $(TARGET)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.cpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(RC_OBJECT): src/app.rc src/app.ico src/explosion_sheet.png src/resource.h | $(BUILD_DIR)
	$(WINDRES) $< -O coff -o $@

$(TARGET): $(OBJECTS) $(RC_OBJECT)
	$(CXX) $(OBJECTS) $(RC_OBJECT) -o $(TARGET) $(LDFLAGS) $(LIBS)
	@echo "Built $(TARGET)"

clean:
	rm -rf $(BUILD_DIR)

test:
	$(CXX) -std=c++17 -O2 -Wall -Wextra -Isrc src/expr_tree.cpp src/evaluator.cpp src/layout.cpp src/workspace.cpp src/graph.cpp src/gpu_graph.cpp src/symbolic.cpp tests/calculator_tests.cpp -lgdi32 -ld3d11 -ldxgi -o $(BUILD_DIR)/calculator_tests.exe
	$(BUILD_DIR)/calculator_tests.exe
	$(CXX) -std=c++17 -O2 -Wall -Wextra -Isrc src/expr_tree.cpp src/evaluator.cpp src/layout.cpp src/workspace.cpp src/graph.cpp src/gpu_graph.cpp src/symbolic.cpp tests/gpu_graph_tests.cpp -lgdi32 -ld3d11 -ldxgi -o $(BUILD_DIR)/gpu_graph_tests.exe
	$(BUILD_DIR)/gpu_graph_tests.exe
