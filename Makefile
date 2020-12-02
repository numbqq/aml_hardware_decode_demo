all:
	gcc -o esplayer esplayer.c  -lamcodec -lamadec -lamavutils -lasound -lpthread

clean:
	rm -rf esplayer

