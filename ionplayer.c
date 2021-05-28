/**************************************************
* example based on amcodec
**************************************************/
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
#include <stdbool.h>
#include <ctype.h>
#include <codec.h>
#include <amvideo.h>
#include <ion/IONmem.h>


#define READ_SIZE (64 * 1024)
#define EXTERNAL_PTS    (1)
#define SYNC_OUTSIDE    (2)
#define UNIT_FREQ       96000
#define PTS_FREQ        90000
#define AV_SYNC_THRESH    PTS_FREQ*30
#define MESON_BUFFER_SIZE 4

static codec_para_t v_codec_para;
static codec_para_t *vpcodec;
#ifdef AUDIO_ES
static codec_para_t a_codec_para;
static codec_para_t *apcodec;
#endif
static codec_para_t *pcodec;
static char *filename;
FILE* fp = NULL;
FILE* yuv = NULL;
static int axis[8] = {0};
struct amvideo_dev *amvideo;
int g_double_write_mode = 0;

struct out_buffer_t {
    int index;
    int size;
    bool own_by_v4l;
    void *ptr;
    IONMEM_AllocParams buffer;
} vbuffer[MESON_BUFFER_SIZE];

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

void set_double_write_mode(char *path)
{
	g_double_write_mode = amsysfs_get_sysfs_int(path);
	amsysfs_set_sysfs_int(path, 16);
}

void reset_double_write_mode(char *path)
{
	amsysfs_set_sysfs_int(path, g_double_write_mode);
}

int parse_para(const char *para, int para_num, int *result)
{
    char *endp;
    const char *startp = para;
    int *out = result;
    int len = 0, count = 0;

    if (!startp) {
        return 0;
    }

    len = strlen(startp);

    do {
        //filter space out
        while (startp && (isspace(*startp) || !isgraph(*startp)) && len) {
            startp++;
            len--;
        }

        if (len == 0) {
            break;
        }

        *out++ = strtol(startp, &endp, 0);

        len -= endp - startp;
        startp = endp;
        count++;

    } while ((endp) && (count < para_num) && (len > 0));

    return count;
}

static void FreeBuffers()
{
    int i;
    for (i = 0; i < MESON_BUFFER_SIZE; i++) {
        if (vbuffer[i].ptr) {
            munmap(vbuffer[i].ptr, vbuffer[i].size);
            CMEM_free(&vbuffer[i].buffer);
        }
    }
}
static int AllocBuffers(int width, int height)
{
    int i, size, ret;
    CMEM_init();
    size = width * height * 3 / 2;
    for (i = 0; i < MESON_BUFFER_SIZE; i++) {
        ret = CMEM_alloc(size, &vbuffer[i].buffer);
        if (ret < 0) {
            printf("CMEM_alloc failed\n");
            FreeBuffers();
            goto fail;
        }
        vbuffer[i].index = i;
        vbuffer[i].size = size;
        vbuffer[i].ptr = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, vbuffer[i].buffer.mImageFd, 0);
    }
fail:
    return ret;
}
static int ionvideo_init(int width, int height)
{
    int i, ret;


    amsysfs_set_sysfs_str("/sys/class/vfm/map", "rm default");
    amsysfs_set_sysfs_str("/sys/class/vfm/map",
               "add default decoder ionvideo");

    AllocBuffers(width, height);

    amvideo = new_amvideo(FLAGS_V4L_MODE);
    if (!amvideo) {
        printf("amvideo create failed\n");
        ret = -ENODEV;
        goto fail;
    }
    amvideo->display_mode = 0;
    amvideo->use_frame_mode = 0;

    ret = amvideo_init(amvideo, 0, width, height,
            V4L2_PIX_FMT_NV12, MESON_BUFFER_SIZE);
    if (ret < 0) {
        printf("amvideo_init failed\n");
        amvideo_release(amvideo);
        goto fail;
    }
    ret = amvideo_start(amvideo);
    if (ret < 0) {
        amvideo_release(amvideo);
        goto fail;
    }
    for (i = 0; i < MESON_BUFFER_SIZE; i++) {
        vframebuf_t vf;
        vf.fd = vbuffer[i].buffer.mImageFd;
        vf.length = vbuffer[i].buffer.size;
        vf.index = vbuffer[i].index;
        ret = amlv4l_queuebuf(amvideo, &vf);
    }
fail:
    return ret;
}

static void ionvideo_close()
{
    amvideo_stop(amvideo);
    amvideo_release(amvideo);
}

static void signal_handler(int signum)
{
    printf("Get signum=%x\n", signum);
#ifdef AUDIO_ES
    codec_close(apcodec);
#endif
    codec_close(vpcodec);
    fclose(fp);
    signal(signum, SIG_DFL);
    raise(signum);
    ionvideo_close();
    FreeBuffers();
	if (vpcodec->video_type == VFORMAT_HEVC)
		reset_double_write_mode("/sys/module/amvdec_h265/parameters/double_write_mode");
}

int main(int argc, char *argv[])
{
    int ret = CODEC_ERROR_NONE;
    char buffer[READ_SIZE];

    int len = 0;
    int size = READ_SIZE;
    uint32_t Readlen;
    uint32_t isize;
    struct buf_status vbuf;
	char double_write_mode_node[64];

    if (argc < 6) {
        printf("Corret command: ionplayer <filename> <width> <height> <fps> <format(1:mpeg4 2:h264 11:hevc)> [subformat for mpeg4]\n");
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
	}
	else if (vpcodec->video_type == VFORMAT_HEVC) {
		vpcodec->am_sysinfo.format = VIDEO_DEC_FORMAT_HEVC;
		vpcodec->am_sysinfo.param = (void *)(EXTERNAL_PTS | SYNC_OUTSIDE);
		set_double_write_mode("/sys/module/amvdec_h265/parameters/double_write_mode");
    } else if (vpcodec->video_type == VFORMAT_MPEG4) {
        if (argc < 7) {
            printf("No subformat for mpeg4, take the default VIDEO_DEC_FORMAT_MPEG4_5\n");
            vpcodec->am_sysinfo.format = VIDEO_DEC_FORMAT_MPEG4_5;
        } else {
            vpcodec->am_sysinfo.format = atoi(argv[6]);
        }
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

    if ((yuv = fopen("/tmp/yuv.yuv", "wb")) == NULL) {
        printf("/tmp/yuv.yuv dump open file error!\n");
        return -1;
    }

    ionvideo_init(vpcodec->am_sysinfo.width, vpcodec->am_sysinfo.height);
#ifdef AUDIO_ES
    ret = codec_init(apcodec);
    if (ret != CODEC_ERROR_NONE) {
        printf("codec init failed, ret=-0x%x", -ret);
        return -1;
    }
#endif

    ret = codec_init(vpcodec);
    if (ret != CODEC_ERROR_NONE) {
        printf("codec init failed, ret=-0x%x", -ret);
        return -1;
    }
    printf("video codec ok!\n");

    //codec_set_cntl_avthresh(vpcodec, AV_SYNC_THRESH);
    //codec_set_cntl_syncthresh(vpcodec, 0);

    set_tsync_enable(0);

    pcodec = vpcodec;
    while (!feof(fp)) {
        Readlen = fread(buffer, 1, READ_SIZE, fp);
        //printf("Readlen %d\n", Readlen);
        if (Readlen <= 0) {
            printf("read file error!\n");
            rewind(fp);
        }

        isize = 0;
        do {
            vframebuf_t vf;
            ret = amlv4l_dequeuebuf(amvideo, &vf);
            if (ret >= 0) {
                printf("vf idx%d pts 0x%x\n", vf.index, vf.pts);
                fwrite(vbuffer[vf.index].ptr, vbuffer[vf.index].size, 1, yuv);
				fflush(yuv);
                ret = amlv4l_queuebuf(amvideo, &vf);
                if (ret < 0) {
                    //printf("amlv4l_queuebuf %d\n", ret);
                }
            } else {
                //printf("amlv4l_dequeuebuf %d\n", ret);
            }
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

            //printf("ret %d, isize %d\n", ret, isize);
        } while (isize < Readlen);

        signal(SIGCHLD, SIG_IGN);
        signal(SIGTSTP, SIG_IGN);
        signal(SIGTTOU, SIG_IGN);
        signal(SIGTTIN, SIG_IGN);
        signal(SIGHUP, signal_handler);
        signal(SIGTERM, signal_handler);
        signal(SIGSEGV, signal_handler);
        signal(SIGINT, signal_handler);
        signal(SIGQUIT, signal_handler);
    }

    do {
        vframebuf_t vf;
        ret = codec_get_vbuf_state(pcodec, &vbuf);
        if (ret != 0) {
            printf("codec_get_vbuf_state error: %x\n", -ret);
            goto error;
        }

        ret = amlv4l_dequeuebuf(amvideo, &vf);
        if (ret >= 0) {
            printf("vf idx%d pts 0x%x\n", vf.index, vf.pts);
            fwrite(vbuffer[vf.index].ptr, vbuffer[vf.index].size, 1, yuv);
			fflush(yuv);
            ret = amlv4l_queuebuf(amvideo, &vf);
            if (ret < 0) {
                //printf("amlv4l_queuebuf %d\n", ret);
            }
        } else {
            //printf("amlv4l_dequeuebuf %d\n", ret);
        }
    } while (vbuf.data_len > 0x100);

error:
#ifdef AUDIO_ES
    codec_close(apcodec);
#endif
    codec_close(vpcodec);
    fclose(fp);
    fclose(yuv);
    ionvideo_close();
    FreeBuffers();
	if (vpcodec->video_type == VFORMAT_HEVC)
		reset_double_write_mode("/sys/module/amvdec_h265/parameters/double_write_mode");
    return 0;
}

