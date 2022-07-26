CC = g++
CLAGS=-Wall
LINK = -lpthread

all: network_send network_recv

network_send: network_send.c
	${CC} ${CLAGS} network_send.c ${LINK} -o network_send

network_recv: network_recv.c
	${CC} ${CLAGS} network_recv.c ${LINK} -o network_recv

.PHONY: clean, cleanall

cleanall: clean

clean:
	rm network_send