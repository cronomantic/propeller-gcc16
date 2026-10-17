# SPDX-License-Identifier: GPL-3.0-or-later
# The Propeller 1 emulator used by tests/run.sh.
#
#   make -C emu                 # emu/build/p1run
#   make -C emu test            # its own tests
#
# C++17, no libraries.  On Windows the programs are linked statically.

# make's built-in default for CXX is not always g++ (e.g. winlibs)
ifeq ($(origin CXX),default)
  CXX := g++
endif
BUILD    ?= build
CXXFLAGS ?= -O2
CXXFLAGS += -std=c++17 -Wall -Wextra
ifeq ($(OS),Windows_NT)
  EXE     := .exe
  LDFLAGS += -static
endif

CORE  := p1 uart
TESTS := test_main test_p1 test_ctr

all: $(BUILD)/p1run$(EXE)

$(BUILD)/p1run$(EXE): $(CORE:%=$(BUILD)/obj/%.o) $(BUILD)/obj/p1run.o
	$(CXX) $(LDFLAGS) -o $@ $^

$(BUILD)/emu_tests$(EXE): $(CORE:%=$(BUILD)/obj/%.o) $(TESTS:%=$(BUILD)/obj/%.o)
	$(CXX) $(LDFLAGS) -o $@ $^

test: $(BUILD)/emu_tests$(EXE)
	$(BUILD)/emu_tests$(EXE)

$(BUILD)/obj/%.o: src/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/obj/%.o: tests/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

clean:
	rm -rf $(BUILD)

-include $(wildcard $(BUILD)/obj/*.d)

.PHONY: all test clean
