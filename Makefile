# CaaL - Container as a Login
# https://github.com/douxxtech/caal

CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra -pedantic
PREFIX ?= /usr/local

SRC_DIR = src
CAALSH_SRC = $(SRC_DIR)/caalsh.c $(SRC_DIR)/lib/tomlc17.c $(SRC_DIR)/lib/pty_bridge.c $(SRC_DIR)/lib/session_disk.c $(SRC_DIR)/lib/caald_client.c
CAALSH_OBJ = caalsh

DAEMON_SRC = $(SRC_DIR)/caald.c
DAEMON_OBJ = caald

CONTROLLER_SRC = $(SRC_DIR)/caalctl.c $(SRC_DIR)/lib/caald_client.c
CONTROLLER_OBJ = caalctl

NEWCAAL_SRC = scripts/newcaal.sh
DELCAAL_SRC = scripts/delcaal.sh

.PHONY: all install uninstall clean

all: $(CAALSH_OBJ) $(DAEMON_OBJ) $(CONTROLLER_OBJ)

$(CAALSH_OBJ): $(CAALSH_SRC) $(SRC_DIR)/config.h
	$(CC) $(CFLAGS) -I$(SRC_DIR) -o $@ $(CAALSH_SRC)

$(DAEMON_OBJ): $(DAEMON_SRC) $(SRC_DIR)/lib/caald_proto.h
	$(CC) $(CFLAGS) -I$(SRC_DIR) -o $@ $(DAEMON_SRC)

$(CONTROLLER_OBJ): $(CONTROLLER_SRC) $(SRC_DIR)/lib/caald_proto.h
	$(CC) $(CFLAGS) -I$(SRC_DIR) -o $@ $(CONTROLLER_SRC)

install: all
	install -Dm4755 $(CAALSH_OBJ) $(DESTDIR)$(PREFIX)/bin/caalsh
	install -Dm755  $(DAEMON_OBJ) $(DESTDIR)$(PREFIX)/bin/caald
	install -Dm755  $(CONTROLLER_OBJ) $(DESTDIR)$(PREFIX)/bin/caalctl
	install -Dm755  $(NEWCAAL_SRC) $(DESTDIR)$(PREFIX)/bin/newcaal
	install -Dm755  $(DELCAAL_SRC) $(DESTDIR)$(PREFIX)/bin/delcaal
	@echo "Installed"

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/caalsh
	rm -f $(DESTDIR)$(PREFIX)/bin/caald
	rm -f $(DESTDIR)$(PREFIX)/bin/caalctl
	rm -f $(DESTDIR)$(PREFIX)/bin/newcaal
	rm -f $(DESTDIR)$(PREFIX)/bin/delcaal
	@echo "Uninstalled"

clean:
	rm -f $(CAALSH_OBJ) $(DAEMON_OBJ)