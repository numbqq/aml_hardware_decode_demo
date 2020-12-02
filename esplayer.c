/*
 * Copyright (c) 2014 Amlogic, Inc. All rights reserved.
 * Copyright (c) 2020 Wesion, Inc. All rights reserved.
 *
 * This source code is subject to the terms and conditions defined in the
 * file 'LICENSE' which is part of this source code package.
 *
 * Description:
 */


/**************************************************
* example based on amcodec
**************************************************/
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
#include <codec.h>
#include <stdbool.h>
#include <ctype.h>
#include <unistd.h>

#define READ_SIZE (64 * 1024)
#define EXTERNAL_PTS    (1)
#define SYNC_OUTSIDE    (2)
#define UNIT_FREQ       96000
#define PTS_FREQ        90000
#define AV_SYNC_THRESH    PTS_FREQ*30

static codec_para_t v_codec_para;
static codec_para_t a_codec_para;
static codec_para_t *pcodec, *apcodec, *vpcodec;
static char *filename;
FILE* fp = NULL;

int osd_blank(char *path, int cmd)
{
	int fd;
	char  bcmd[16];
	fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0644);

	if (fd >= 0) {
		sprintf(bcmd, "%d", cmd);
		write(fd, bcmd, strlen(bcmd));
		close(fd);
		return 0;
	}

	return -1;
}

void init_display(void)
{
	osd_blank("/sys/class/graphics/fb0/blank", 1);
	osd_blank("/sys/class/graphics/fb1/blank", 1);
}

void restore_display(void)
{
	osd_blank("/sys/class/graphics/fb0/blank", 0);
	osd_blank("/sys/class/graphics/fb1/blank", 0);
}

int set_tsync_enable(int enable)
{
	int fd;
	char *path = "/sys/class/tsync/enable";
	char  bcmd[16];
	fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0644);
	if (fd >= 0) {
		sprintf(bcmd, "%d", enable);
		write(fd, bcmd, strlen(bcmd));
		close(fd);
		return 0;
	}

	return -1;
}

static void signal_handler(int signum)
{
	printf("Get signum=%x\n", signum);
#ifdef AUDIO_ES
	codec_close(apcodec);
#endif
	codec_close(vpcodec);
	fclose(fp);
	restore_display();
	signal(signum, SIG_DFL);
	raise(signum);
}

int main(int argc, char *argv[])
{
	int ret = CODEC_ERROR_NONE;
	char buffer[READ_SIZE];

	int len = 0;
	int size = READ_SIZE;
	int Readlen;
	int isize;
	struct buf_status vbuf;

	if (argc < 6) {
		printf("Corret command: esplayer <filename> <width> <height> <fps> <format(1:mpeg4 2:h264 6:vc1 11:hevc)> [subformat for mpeg4/vc1]\n");
		return -1;
	}

#ifdef AUDIO_ES
	apcodec = &a_codec_para;
	memset(apcodec, 0, sizeof(codec_para_t));
#endif

	vpcodec = &v_codec_para;
	memset(vpcodec, 0, sizeof(codec_para_t));

	vpcodec->has_video = 1;
	vpcodec->video_type = atoi(argv[5]);
	if (vpcodec->video_type == VFORMAT_H264) {
		vpcodec->am_sysinfo.format = VIDEO_DEC_FORMAT_H264;
		vpcodec->am_sysinfo.param = (void *)(EXTERNAL_PTS | SYNC_OUTSIDE);
	} else if (vpcodec->video_type == VFORMAT_VC1) {
		if (argc < 7) {
			printf("No subformat for vc1, take the default VIDEO_DEC_FORMAT_WVC1\n");
			vpcodec->am_sysinfo.format = VIDEO_DEC_FORMAT_WVC1;
		} else {
			vpcodec->am_sysinfo.format = atoi(argv[6]);
		}
	} else if (vpcodec->video_type == VFORMAT_MPEG4) {
		if (argc < 7) {
			printf("No subformat for mpeg4, take the default VIDEO_DEC_FORMAT_MPEG4_5\n");
			vpcodec->am_sysinfo.format = VIDEO_DEC_FORMAT_MPEG4_5;
		} else {
			vpcodec->am_sysinfo.format = atoi(argv[6]);
		}
	} else if (vpcodec->video_type == VFORMAT_HEVC) {
		vpcodec->am_sysinfo.format = VIDEO_DEC_FORMAT_HEVC;
		vpcodec->am_sysinfo.param = (void *)(EXTERNAL_PTS | SYNC_OUTSIDE);
	} else {
		printf("unsupported video type: %d\n", vpcodec->video_type);
		return -1;
	}

	vpcodec->stream_type = STREAM_TYPE_ES_VIDEO;
	vpcodec->am_sysinfo.rate = 96000 / atoi(argv[4]);
	vpcodec->am_sysinfo.height = atoi(argv[3]);
	vpcodec->am_sysinfo.width = atoi(argv[2]);
	vpcodec->has_audio = 0;
	vpcodec->noblock = 0;

#ifdef AUDIO_ES
	apcodec->audio_type = AFORMAT_MPEG;
	apcodec->stream_type = STREAM_TYPE_ES_AUDIO;
	apcodec->audio_pid = 0x1023;
	apcodec->has_audio = 1;
	apcodec->audio_channels = 2;
	apcodec->audio_samplerate = 48000;
	apcodec->noblock = 0;
	apcodec->audio_info.channels = 2;
	apcodec->audio_info.sample_rate = 48000;
#endif

	printf("\n*********CODEC PLAYER DEMO************\n\n");
	filename = argv[1];
	printf("file %s to be played\n", filename);

	if ((fp = fopen(filename, "rb")) == NULL) {
		printf("open file error!\n");
		return -1;
	}

	init_display();

#ifdef AUDIO_ES
	ret = codec_init(apcodec);
		if (ret != CODEC_ERROR_NONE) {
			printf("codec init failed, ret=-0x%x", -ret);
			goto close_fp;
	}
#endif

	ret = codec_init(vpcodec);
	if (ret != CODEC_ERROR_NONE) {
		printf("codec init failed, ret=-0x%x", -ret);
#ifdef AUDIO_ES
		goto close_audio_fp;
#else
		goto close_fp;
#endif
	}
	printf("video codec ok!\n");

	//codec_set_cntl_avthresh(vpcodec, AV_SYNC_THRESH);
	//codec_set_cntl_syncthresh(vpcodec, 0);

	set_tsync_enable(0);

	signal(SIGCHLD, SIG_IGN);
	signal(SIGTSTP, SIG_IGN);
	signal(SIGTTOU, SIG_IGN);
	signal(SIGTTIN, SIG_IGN);
	signal(SIGHUP, signal_handler);
	signal(SIGTERM, signal_handler);
	signal(SIGSEGV, signal_handler);
	signal(SIGINT, signal_handler);
	signal(SIGQUIT, signal_handler);

	pcodec = vpcodec;
	while (!feof(fp)) {
		Readlen = fread(buffer, 1, READ_SIZE, fp);
		if (Readlen <= 0) {
			printf("read file error!\n");
			rewind(fp);
		}

		isize = 0;
		do {
				ret = codec_write(pcodec, buffer + isize, Readlen);
				if (ret < 0) {
					if (errno != EAGAIN) {
						printf("write data failed, errno %d\n", errno);
						goto error;
					} else {
						continue;
					}
				} else {
					isize += ret;
				}
		} while (isize < Readlen);
	}

	do {
		ret = codec_get_vbuf_state(pcodec, &vbuf);
		if (ret != 0) {
			printf("codec_get_vbuf_state error: %x\n", -ret);
			goto error;
		}
	} while (vbuf.data_len > 0x100);

error:
	codec_close(vpcodec);
#ifdef AUDIO_ES
close_audio_fp:
	codec_close(apcodec);
#endif
close_fp:
	fclose(fp);

	restore_display();

	return 0;
}
