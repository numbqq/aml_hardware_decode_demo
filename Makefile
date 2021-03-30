all:
	gcc -o esplayer esplayer.c  -lamcodec -lamadec -lamavutils -lpthread

clean:
	rm -rf esplayer

