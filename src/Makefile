#
# Copyright (c) 2015 Justin Liu
# Author: Justin Liu <rssnsj@gmail.com>
# https://github.com/rssnsj/minivtun
#

ifeq ($(PREFIX),)
PREFIX := $(shell [ -d /opt/local ] && echo /opt/local || echo /usr )
endif

CC ?= gcc
CFLAGS += -Wall -I/opt/local/include 
HEADERS = minivtun.h library.h list.h jhash.h
LDFLAGS += -L/opt/local/lib -lcrypto

minivtun: minivtun.o library.o server.o client.o client_route.o
	$(CC) $(LDFLAGS) -o $@ $^ -lcrypto

%.o: %.c $(HEADERS)
	$(CC) $(CFLAGS) -c -o $@ $<

install: minivtun
	cp -f minivtun $(PREFIX)/sbin/

clean:
	rm -f minivtun *.o

