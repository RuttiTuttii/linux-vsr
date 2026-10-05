CC ?= gcc
CFLAGS ?= -O3 -Wall -Wextra -fPIC -fvisibility=hidden
LDFLAGS ?= -shared -ldl

PREFIX ?= /usr/local
LIBDIR ?= $(PREFIX)/lib

SRCDIR = src
BUILDDIR = build

TARGET = libzen_vsr.so
SOURCES = $(SRCDIR)/hook_egl.c
OBJECTS = $(BUILDDIR)/hook_egl.o

.PHONY: all clean install uninstall

all: $(TARGET)

$(BUILDDIR):
	mkdir -p $(BUILDDIR)

$(BUILDDIR)/%.o: $(SRCDIR)/%.c | $(BUILDDIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(TARGET): $(OBJECTS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^

clean:
	rm -rf $(BUILDDIR) $(TARGET)

install: $(TARGET)
	install -d $(DESTDIR)$(LIBDIR)
	install -m 755 $(TARGET) $(DESTDIR)$(LIBDIR)/

uninstall:
	rm -f $(DESTDIR)$(LIBDIR)/$(TARGET)
