CXX ?= g++
CPPFLAGS ?=
CXXFLAGS ?= -O2 -g
WARNINGS := -Wall -Wextra -Wpedantic -Wconversion -Wshadow
THIRD_PARTY_WARNINGS := -Wno-conversion -Wno-shadow
STANDARD := -std=c++17
THREAD_FLAGS := -pthread

COMMON_OBJECTS := standalone_allocator.o standalone_trimmer.o
COMMON_HEADERS := standalone_reclaim.h third_party/dlmalloc.c
BINARIES := poc_b_cursor_rebuild poc_c_active_trim_churn

.PHONY: all clean check-linux compile

all: check-linux $(BINARIES)

# Useful for compiler-only validation on non-Linux hosts. The binaries still
# fail fast at runtime because the experiments require tmpfs and MADV_REMOVE.
compile: $(BINARIES)

check-linux:
	@test "$$(uname -s)" = Linux || { \
		echo "error: PoC-B/C require Linux (tmpfs and MADV_REMOVE)" >&2; \
		exit 1; \
	}

poc_b_cursor_rebuild: poc_b_cursor_rebuild.cc $(COMMON_OBJECTS) standalone_reclaim.h
	$(CXX) $(CPPFLAGS) $(STANDARD) $(CXXFLAGS) $(WARNINGS) $(THREAD_FLAGS) \
		-o $@ poc_b_cursor_rebuild.cc $(COMMON_OBJECTS)

poc_c_active_trim_churn: poc_c_active_trim_churn.cc $(COMMON_OBJECTS) standalone_reclaim.h
	$(CXX) $(CPPFLAGS) $(STANDARD) $(CXXFLAGS) $(WARNINGS) $(THREAD_FLAGS) \
		-o $@ poc_c_active_trim_churn.cc $(COMMON_OBJECTS)

standalone_allocator.o: standalone_allocator.cc $(COMMON_HEADERS)
	$(CXX) $(CPPFLAGS) $(STANDARD) $(CXXFLAGS) $(WARNINGS) \
		$(THIRD_PARTY_WARNINGS) -c -o $@ standalone_allocator.cc

standalone_trimmer.o: standalone_trimmer.cc standalone_reclaim.h
	$(CXX) $(CPPFLAGS) $(STANDARD) $(CXXFLAGS) $(WARNINGS) -c -o $@ $<

clean:
	rm -f $(BINARIES) $(COMMON_OBJECTS)
	rm -rf $(addsuffix .dSYM,$(BINARIES))
