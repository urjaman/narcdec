
narcdec: narcdec.c
	gcc -W -Wall -O2 -std=gnu99 -o narcdec narcdec.c -lz

clean: 
	-rm narcdec

install:
	cp narcdec /usr/bin
