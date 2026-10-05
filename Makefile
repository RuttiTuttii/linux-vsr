CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra -Werror -fPIC -Iinclude -pthread -fstack-protector-strong -D_FORTIFY_SOURCE=2 -fvisibility=hidden
LDFLAGS ?= -shared -ldl -pthread

BUILDDIR = build
SRCDIR = src
TESTDIR = tests

LIB_TARGET = libvsr.so
TEST_TARGET = $(BUILDDIR)/test_runner

SOURCES = $(wildcard $(SRCDIR)/*.c)
OBJECTS = $(patsubst $(SRCDIR)/%.c, $(BUILDDIR)/%.o, $(SOURCES))

TEST_SOURCES = $(wildcard $(TESTDIR)/*.c)
TEST_OBJECTS = $(patsubst $(TESTDIR)/%.c, $(BUILDDIR)/%.o, $(TEST_SOURCES))

# modules needed for tests (excluding hook and main which intercept symbols)
CORE_TEST_OBJECTS = $(BUILDDIR)/config.o $(BUILDDIR)/shaders.o $(BUILDDIR)/patcher.o $(BUILDDIR)/logger.o $(BUILDDIR)/safety.o

.PHONY: all clean test

all: $(LIB_TARGET)

$(BUILDDIR):
	mkdir -p $(BUILDDIR)

$(BUILDDIR)/%.o: $(SRCDIR)/%.c | $(BUILDDIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILDDIR)/%.o: $(TESTDIR)/%.c | $(BUILDDIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(LIB_TARGET): $(OBJECTS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^

test: $(TEST_TARGET)
	./$(TEST_TARGET)

$(TEST_TARGET): $(CORE_TEST_OBJECTS) $(TEST_OBJECTS)
	$(CC) $(CFLAGS) -o $@ $^ -lm

clean:
	rm -rf $(BUILDDIR) $(LIB_TARGET)
