#include "AEConfig.h"

#include "entry.h"

#ifdef AE_OS_WIN
#include <windows.h>
#endif
#include<string>
#include "AE_IO.h"
#include "AE_Macros.h"
#include "AE_EffectCBSuites.h"
#include "AEGP_SuiteHandler.h"

#define PATH_MAX_LEN		1024
#define PIXEL_DEPTH_BYTE	4
#define CACHE_FRAMES		128
#define ERR_NULL(s)			if(!err && (s)==NULL) err|=A_Err_ALLOC
#define MAX_AUDIO_FRAME_SIZE 192000
#define AUDIO_CHANGE_PTS	-2
#define VIDEO_FROM_HEAD		-1
#define MAX_FPS_			1000LL
extern "C" {
	#include <libavcodec/avcodec.h>
	#include <libswscale/swscale.h>
	#include <libavformat/avformat.h>
	#include <libswresample/swresample.h>
	// Note when you modify this struct, consider also modifying IO_FlatFileOutputOptions, defined further down
	typedef struct {
		AVFormatContext	*pFormatCtx, *aFormatCtx;
		A_long			videoIndex, audioIndex;
		AVCodecContext	*pCodecCtx, *aCodecCtx;
		AVStream		*videoStream, *audioStream;
		AVCodec			*pCodec, *aCodec;
		AVFrame			*pFrame, *pFrameRGB, *aFrame;
		uint8_t			*out_buffer;
		//AVPacket		*packet; 
		SwsContext		*img_convert_ctx;
		int64_t			last_pts;
	}ffmpeg_info;
	typedef struct {
		A_char				name[AEGP_MAX_PATH_SIZE];
		A_u_char			hasAudio;
		A_u_char			hasVideo;
		A_u_long			widthLu;					// width of frame in pixels
		A_u_long			heightLu;					// height of frame in pixels
		A_u_long			rowbytesLu;					// total bytes in row, aka width * 4, we strip any row padding
		A_Time				fpsT;
		A_short				bitdepthS;
		A_FpLong			rateF;
		A_long				avg_bit_rateL;
		A_long				block_sizeL;
		A_long				frames_per_blockL;
		AEIO_SndEncoding	encoding;
		AEIO_SndSampleSize	bytes_per_sample;
		AEIO_SndChannels	num_channels;
		A_short				padS;
		A_Time				durationT;
		AEIO_FileSize		size;
		std::string			path;
		ffmpeg_info			ffmpeg;
	}Logo_FileHeader;
	typedef struct {
		char path[PATH_MAX_LEN];
	}Logo_FlatHeader;
}
extern "C" DllExport AEGP_PluginInitFuncPrototype EntryPointFunc;

A_Err
ConstructFunctionBlock(AEIO_FunctionBlock4	*funcs);

static void PRINTXX1(A_long num, char *strt = NULL){
	if (num >= 100000){
		MessageBox(0, "too big", "1", 0);
		return;
	}
	if (num < 0){
		MessageBox(0, "too small", "2", 0);
		return;
	}
	char sss[10];
	sss[0] = (char)(num / 10000 + '0');
	num -= num / 10000 * 10000;
	sss[1] = (char)(num / 1000 + '0');
	num -= num / 1000 * 1000;
	sss[2] = (char)(num / 100 + '0');
	num -= num / 100 * 100;
	sss[3] = (char)(num / 10 + '0');
	sss[4] = (char)(num % 10 + '0');
	sss[5] = 0;
	MessageBox(NULL, strt, sss, 0);
}