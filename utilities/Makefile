all:	econet-bridge econet-monitor econet-imm econet-test

econet-bridge: econet-bridge.o fs.o sockets.o

econet-monitor: econet-monitor.o

econet-imm: econet-imm.o

econet-test: econet-test.o

econet-bridge.o: econet-bridge.c fs.c sockets.c ../include/econet-gpio-consumer.h
	cc -c econet-bridge.c -Wall
	cc -c fs.c -Wall -Wno-pointer-sign 
	cc -c sockets.c -Wall

econet-monitor.o: econet-monitor.c ../include/econet-gpio-consumer.h

econet-imm.o: econet-imm.c ../include/econet-gpio-consumer.h

econet-test.o: econet-test.c ../include/econet-gpio-consumer.h

clean:
	rm -f econet-bridge.o fs.o sockets.o econet-bridge econet-monitor.o econet-monitor econet-imm.o econet-imm econet-test.o econet-test
