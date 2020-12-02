all:
	gcc -o ionplayer ionplayer.c  -lamcodec -lamadec -lamvdec -lamavutils -lpthread

clean:
	rm -rf ionplayer

