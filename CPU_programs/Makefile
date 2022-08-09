CC = g++
CLAGS=-Wall
LINK = -lpthread

all: network_send network_send_slow network_send_per_query network_recv  query_send_thread result_recv_thread

network_send: network_send.c
	${CC} ${CLAGS} network_send.c ${LINK} -o network_send

network_send_slow: network_send_slow.c
	${CC} ${CLAGS} network_send_slow.c ${LINK} -o network_send_slow

network_send_per_query: network_send_per_query.c
	${CC} ${CLAGS} network_send_per_query.c ${LINK} -o network_send_per_query	

network_recv: network_recv.c
	${CC} ${CLAGS} network_recv.c ${LINK} -o network_recv

query_send_thread: query_send_thread.c
	${CC} ${CLAGS} query_send_thread.c ${LINK} -o query_send_thread

result_recv_thread: result_recv_thread.c
	${CC} ${CLAGS} result_recv_thread.c ${LINK} -o result_recv_thread

.PHONY: clean, cleanall

cleanall: clean

clean:
	rm network_send network_send_slow network_send_per_query network_recv  query_send_thread result_recv_thread
