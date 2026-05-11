# CaaL - Container as a Login
# https://github.com/douxxtech/caal

CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra -pedantic
PREFIX ?= /usr/local

SRC_DIR = src
SRC = $(SRC_DIR)/caalsh.c $(SRC_DIR)/lib/tomlc17.c
OBJ = caalsh

NEWCAAL_SRC = scripts/newcaal.sh
DELCAAL_SRC = scripts/delcaal.sh

.PHONY: all install uninstall clean

all: $(OBJ)

$(OBJ): $(SRC) $(SRC_DIR)/config.h
	$(CC) $(CFLAGS) -I$(SRC_DIR) -o $@ $(SRC)

install: all
	install -Dm4755 $(OBJ) $(DESTDIR)$(PREFIX)/bin/caalsh
	install -Dm755  $(NEWCAAL_SRC) $(DESTDIR)$(PREFIX)/bin/newcaal
	install -Dm755  $(DELCAAL_SRC) $(DESTDIR)$(PREFIX)/bin/delcaal
	@echo "Installed"

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/caalsh
	rm -f $(DESTDIR)$(PREFIX)/bin/newcaal
	rm -f $(DESTDIR)$(PREFIX)/bin/delcaal

clean:
	rm -f $(OBJ)