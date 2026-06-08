CC=gcc
CFLAGS=-Wall -O2 -pthread

all: iot_server iot_client bt_stm32_bridge

iot_server: iot_server.c
	$(CC) $(CFLAGS) -o $@ $<

iot_client: iot_client.c
	$(CC) $(CFLAGS) -o $@ $<

bt_stm32_bridge: bt_stm32_bridge.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f iot_server iot_client bt_stm32_bridge *.o
