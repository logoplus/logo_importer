

#define __STDC_CONSTANT_MACROS
#include "Logo_Importer.h"
#include <string>
#include <sstream>
#include<iostream>

#include<windows.h>
#include<vector>
/*	Statics and globals are bad. Never use them in production code.  */


#include <process.h> 

static	AEGP_PluginID			S_mem_id					=	0;

static A_Err DeathHook(	
	AEGP_GlobalRefcon	unused1 ,
	AEGP_DeathRefcon	unused2){

	return A_Err_NONE;
}
static std::string WideByte2Acsi(std::wstring& wstrcode)  
{  
    int asciisize = ::WideCharToMultiByte(CP_OEMCP, 0, wstrcode.c_str(), -1, NULL, 0, NULL, NULL);  
    std::vector<char> resultstring(asciisize);  
    int convresult =::WideCharToMultiByte(CP_OEMCP, 0, wstrcode.c_str(), -1, &resultstring[0], asciisize, NULL, NULL);  
    return std::string(&resultstring[0]);  
}  
static std::wstring UTF16_2_WSTRING(const A_UTF16Char *filepath){
	std::wstring unicode=L"";
	const A_UTF16Char *p;
	for (p = filepath; *p; p++){
		unicode += (wchar_t)*p;
	}
	return unicode;
}

static A_Err
ReadFileHeader(
	AEIO_BasicData		*basic_dataP,
	Logo_FileHeader		*file,
	const char			*filepath){
	file->hasVideo = FALSE;
	A_Err				err =	A_Err_NONE;
	ffmpeg_info			*ff	=	&file->ffmpeg;
	AEGP_SuiteHandler	suites(basic_dataP->pica_basicP);
	if (avformat_open_input(&ff->pFormatCtx, /*ascii_path*/filepath, NULL, NULL) != 0){
		//printf("Couldn't open input stream.\n");
		return A_Err_STRUCT;
	}
	//test
	
	if (avformat_find_stream_info(ff->pFormatCtx, NULL)<0){
		//printf("Couldn't find stream information.\n");
		return A_Err_STRUCT;
	}
	file->avg_bit_rateL = ff->pFormatCtx->bit_rate;
	file->size = ff->pFormatCtx->bit_rate*ff->pFormatCtx->duration / AV_TIME_BASE / 8;
	file->durationT.value = static_cast<A_long>(ff->pFormatCtx->duration / 10000);
	//file->durationT.value	=	static_cast<A_long>(ff->videoStream->duration*av_q2d(ff->videoStream->time_base) * 100);
	file->durationT.scale = 100;
	ff->videoIndex = -1;
	for (A_long i = 0; i < ff->pFormatCtx->nb_streams; i++)
		if (ff->pFormatCtx->streams[i]->codec->codec_type == AVMEDIA_TYPE_VIDEO){
			ff->videoIndex = i;
			break;
		}
	if (ff->videoIndex != -1){
		//PRINTXX1(ff->videoIndex,"videoindex");
		ff->videoStream = ff->pFormatCtx->streams[ff->videoIndex];
		if (ff->videoStream->r_frame_rate.num <= ff->videoStream->r_frame_rate.den * MAX_FPS_){

			ff->pCodecCtx = ff->videoStream->codec;
			ff->pCodec = avcodec_find_decoder(ff->pCodecCtx->codec_id);
			if (ff->pCodec == NULL){
				//printf("Codec not found.\n");
				return A_Err_STRUCT;
			}
			if (avcodec_open2(ff->pCodecCtx, ff->pCodec, NULL) < 0){
				//printf("Could not open codec.\n");
				return A_Err_STRUCT;
			}
			//std::cout << "videoStream->duration" << ff->videoStream->duration*av_q2d(ff->videoStream->time_base) << std::endl;
			file->hasVideo = TRUE;
			file->widthLu = ff->pCodecCtx->width;
			file->heightLu = ff->pCodecCtx->height;
			file->rowbytesLu = file->widthLu * 4;
			file->fpsT.value = ff->videoStream->r_frame_rate.num;
			file->fpsT.scale = ff->videoStream->r_frame_rate.den;
		//	PRINTXX1(file->fpsT.value, "r_frame_rate.num");
		//	PRINTXX1(file->fpsT.scale, "r_frame_rate.den");
			file->bitdepthS = 32;
			if ((ff->pFrame = av_frame_alloc()) == NULL)
				err |= A_Err_ALLOC;
			if ((ff->pFrameRGB = av_frame_alloc()) == NULL)
				err |= A_Err_ALLOC;
			ff->out_buffer = (uint8_t *)av_malloc(avpicture_get_size(PIX_FMT_ARGB, ff->pCodecCtx->width, ff->pCodecCtx->height));
			avpicture_fill((AVPicture *)ff->pFrameRGB, ff->out_buffer, PIX_FMT_ARGB, ff->pCodecCtx->width, ff->pCodecCtx->height);
			ff->last_pts = -1;
		}
	}

	

	ff->audioIndex = -1;
	for (A_long i = 0; i < ff->pFormatCtx->nb_streams; i++)
		if (ff->pFormatCtx->streams[i]->codec->codec_type == AVMEDIA_TYPE_AUDIO){
			ff->audioIndex = i;
			break;
		}
	if (ff->audioIndex == -1)
		file->hasAudio = FALSE;
	else{
		file->hasAudio	= TRUE;
		ff->audioStream = ff->pFormatCtx->streams[ff->audioIndex];
		ff->aCodecCtx	= ff->audioStream->codec;
		ff->aCodec		= avcodec_find_decoder(ff->aCodecCtx->codec_id);
		if (ff->aCodec == NULL){
			//printf("Codec not found.\n");
			return A_Err_STRUCT;
		}
		if (avcodec_open2(ff->aCodecCtx, ff->aCodec, NULL)<0){
			//printf("Could not open codec.\n");
			return A_Err_STRUCT;
		}
		file->num_channels = AEIO_SndChannels_STEREO;
		file->rateF = ff->aCodecCtx->sample_rate;
		file->bytes_per_sample = AEIO_SS_4;
		file->encoding = AEIO_E_SIGNED_FLOAT;
		if ((ff->aFrame = av_frame_alloc()) == NULL){
			return A_Err_STRUCT;
		}

	}
	return err;
}


static A_Err	
Logo_InitInSpecFromFile(
	AEIO_BasicData		*basic_dataP,
	const A_UTF16Char	*file_pathZ,
	AEIO_InSpecH		specH)
{ 

	/*	Read the file referenced by the path. Use the 
		file information to set all fields of the AEIO_InSpecH.
	*/

	A_Err err						=	A_Err_NONE,
		  err2						=	A_Err_NONE;

	AEIO_Handle		optionsH		=	NULL,
					old_optionsH	=	NULL;
	Logo_FileHeader	*headerP;

	AEGP_SuiteHandler	suites(basic_dataP->pica_basicP);

	if (!err) {
		/*		What are we doing here? 

			+	Allocate a new OptionsH to hold our file header info.
			+	Lock it in memory, copy our local header into the OptionsH.
			+	If there's an old OptionsH, get rid of it. 
			+	Unlock handle so AE can move it at will.
		
		*/
		ERR(suites.MemorySuite1()->AEGP_NewMemHandle(	S_mem_id,
														"Logo_Importer optionsH", 
														sizeof(Logo_FileHeader), 
														AEGP_MemFlag_CLEAR, 
														&optionsH));
													
		if (optionsH){
			ERR(suites.MemorySuite1()->AEGP_LockMemHandle(optionsH, reinterpret_cast<void **>(&headerP)));
		}	
		if (!err){
			headerP->path = WideByte2Acsi(UTF16_2_WSTRING(file_pathZ));
			ERR(ReadFileHeader(basic_dataP, headerP, headerP->path.data()));

			ERR(suites.IOInSuite4()->AEGP_SetInSpecOptionsHandle(	specH, 
																	optionsH, 
																	reinterpret_cast<void **>(&old_optionsH)));
		
			//	Do NOT free the old options handle. There is old code
			//	in the bowels of AE that does a flat (bytecopy) of the 
			//	input to output OptionsH in the case of a sync.
			/*
			if (old_optionsH){
				ERR(suites.MemorySuite1()->AEGP_FreeMemHandle(old_optionsH));
			}*/
		}

		/*	
			Set specH information based on what we (pretended to) read from the file.
		*/
		ERR(suites.IOInSuite4()->AEGP_SetInSpecSize(specH, headerP->size));
		ERR(suites.IOInSuite4()->AEGP_SetInSpecDuration(specH, &(headerP->durationT)));
		if (headerP->hasVideo){
			ERR(suites.IOInSuite4()->AEGP_SetInSpecDepth(specH, headerP->bitdepthS)); // always 32 bits for .fak files

			ERR(suites.IOInSuite4()->AEGP_SetInSpecDimensions(	specH,
																static_cast<A_long>(headerP->widthLu),
																static_cast<A_long>(headerP->heightLu)));
			ERR(suites.IOInSuite4()->AEGP_SetInSpecNativeFPS(specH, FLOAT2FIX(static_cast<float>(headerP->fpsT.value) / headerP->fpsT.scale)));
		}
		
		if (!err && headerP->bitdepthS == 32){
			AEIO_AlphaLabel	alpha;
			AEFX_CLR_STRUCT(alpha);

			alpha.alpha		=	AEIO_Alpha_STRAIGHT;
			alpha.flags		=	AEIO_AlphaPremul;
			alpha.version	=	AEIO_AlphaLabel_VERSION;
			alpha.red		=	0;
			alpha.green		=	0;
			alpha.blue		=	0;
			
			err = suites.IOInSuite4()->AEGP_SetInSpecAlphaLabel(specH, &alpha);
		}
		if (!err) {
			FIEL_Label	label;
			AEFX_CLR_STRUCT(label);
			label.order		=	FIEL_Order_LOWER_FIRST;
			label.type		=	FIEL_Type_FRAME_RENDERED;
			label.version	=	FIEL_Label_VERSION;
			label.signature	=	FIEL_Tag;

			ERR(suites.IOInSuite4()->AEGP_SetInSpecInterlaceLabel(specH, &label));
		}

		if (!err && headerP->hasAudio)
		{
			ERR(suites.IOInSuite4()->AEGP_SetInSpecSoundRate(specH, headerP->rateF));
			ERR(suites.IOInSuite4()->AEGP_SetInSpecSoundEncoding(specH, headerP->encoding));
			ERR(suites.IOInSuite4()->AEGP_SetInSpecSoundSampleSize(specH, headerP->bytes_per_sample));
			ERR(suites.IOInSuite4()->AEGP_SetInSpecSoundChannels(specH, headerP->num_channels));
		}

		ERR2(suites.MemorySuite1()->AEGP_UnlockMemHandle(optionsH));
	}
	return err;
}

static A_Err
Logo_InitInSpecInteractive(
	AEIO_BasicData	*basic_dataP,
	AEIO_InSpecH	specH)
{ 
	return A_Err_NONE; 
};

static A_Err
Logo_DisposeInSpec(
	AEIO_BasicData	*basic_dataP,
	AEIO_InSpecH	specH)
{ 
	//PRINTXX1(0, "Dispose");
	A_Err				err			=	A_Err_NONE, 
						err2		=	A_Err_NONE;
	AEIO_Handle			optionsH	=	NULL;
	Logo_FileHeader		*headerP	=	NULL;
	ffmpeg_info			*ff			=	NULL;
	AEGP_SuiteHandler	suites(basic_dataP->pica_basicP);	
	ERR(suites.IOInSuite4()->AEGP_GetInSpecOptionsHandle(specH, reinterpret_cast<void**>(&optionsH)));
	
	if (optionsH) {
		ERR(suites.MemorySuite1()->AEGP_LockMemHandle(optionsH, reinterpret_cast<void **>(&headerP)));
		ff = &headerP->ffmpeg;
		if (headerP->hasVideo){
			avcodec_flush_buffers(ff->pCodecCtx);
			av_free(ff->out_buffer);
			av_frame_free(&ff->pFrameRGB);
			av_frame_free(&ff->pFrame);
			avcodec_close(ff->pCodecCtx);
		}
		if (headerP->hasAudio){
			avcodec_flush_buffers(ff->aCodecCtx);
			avcodec_close(ff->aCodecCtx);
		}
		avformat_close_input(&ff->pFormatCtx);
		ERR2(suites.MemorySuite1()->AEGP_UnlockMemHandle(optionsH));
		ERR(suites.MemorySuite1()->AEGP_FreeMemHandle(optionsH));
	}
	return err;
};
/*
static A_Err
Logo_FlattenOptions(
	AEIO_BasicData	*basic_dataP,
	AEIO_InSpecH	specH,
	AEIO_Handle		*flat_optionsPH)
{ 
	A_Err	err		=	A_Err_NONE,
			err2	=	A_Err_NONE; 

	AEIO_Handle			optionsH		= NULL;
	
	Logo_FileHeader		*flat_headerP	= NULL,
						*old_headerP	= NULL;

	AEGP_SuiteHandler	suites(basic_dataP->pica_basicP);	
	
	//	Given the AEIO_InSpecH, acquire the non-flat
	//	options handle and use it to create a flat
	//	version. Do NOT de-allocate the non-flat
	//	options handle!
	
	ERR(suites.IOInSuite4()->AEGP_GetInSpecOptionsHandle(specH, reinterpret_cast<void**>(&optionsH)));

	if (optionsH) {
		ERR(suites.MemorySuite1()->AEGP_LockMemHandle(optionsH, reinterpret_cast<void**>(&old_headerP)));
	}
			
	if (old_headerP){
		ERR(suites.MemorySuite1()->AEGP_NewMemHandle(	S_mem_id,
														"flattened_options", 
														sizeof(Logo_FileHeader), 
														AEGP_MemFlag_CLEAR, 
														flat_optionsPH));
	}

	if (*flat_optionsPH){
		ERR(suites.MemorySuite1()->AEGP_LockMemHandle(*flat_optionsPH, (void**)&flat_headerP));
	}			
	if (!err && flat_headerP) {
		// Here is where you should provide a disk-safe copy of your options data
		ERR(PretendToReadFileHeader(basic_dataP, flat_headerP));
	}

	if (optionsH)
	{
		ERR2(suites.MemorySuite1()->AEGP_UnlockMemHandle(optionsH));
	}
	if (*flat_optionsPH)
	{
		ERR2(suites.MemorySuite1()->AEGP_UnlockMemHandle(*flat_optionsPH));
	}

	return err;
};		

static A_Err
Logo_InflateOptions(
	AEIO_BasicData	*basic_dataP,
	AEIO_InSpecH	specH,
	AEIO_Handle		flat_optionsH)
{ 
	return A_Err_NONE; 
};		

static A_Err
Logo_SynchInSpec(
	AEIO_BasicData	*basic_dataP,
	AEIO_InSpecH	specH, 
	A_Boolean		*changed0)
{ 
	return AEIO_Err_USE_DFLT_CALLBACK;
};
*/
static A_Err	
Logo_GetActiveExtent(
	AEIO_BasicData	*basic_dataP,
	AEIO_InSpecH	specH,				/* >> */
	const A_Time	*tr,				/* >> */
	A_LRect			*extent)			/* << */
{ 
	return AEIO_Err_USE_DFLT_CALLBACK; 
};		

static A_Err	
Logo_GetInSpecInfo(
	AEIO_BasicData	*basic_dataP,
	AEIO_InSpecH	specH, 
	AEIO_Verbiage	*verbiageP)
{ 
	AEGP_SuiteHandler	suites(basic_dataP->pica_basicP);
	
	suites.ANSICallbacksSuite1()->strcpy(verbiageP->name, "");
	suites.ANSICallbacksSuite1()->strcpy(verbiageP->type, "Logo_Importer");
	suites.ANSICallbacksSuite1()->strcpy(verbiageP->sub_type, "Imported by Logo_Importer(ver1.0.7.Prometheus)");

	return A_Err_NONE;
};

static inline 
PF_Pixel *sampleIntegral32(
	PF_EffectWorld *def, 
	int x, 
	int y){
	return (PF_Pixel*)((char*)def->data + (y * def->rowbytes) + (x * sizeof(PF_Pixel)));
}
static void
FfmpegToAE(
	ffmpeg_info		*ff,
	PF_EffectWorld	*wP){
	A_long		i, j,
				perP,perD;
	PF_Pixel	*pixel;
	uint8_t		*data;
	sws_scale(
		ff->img_convert_ctx,
		(const uint8_t* const*)ff->pFrame->data,
		ff->pFrame->linesize,
		0,
		ff->pCodecCtx->height,
		ff->pFrameRGB->data,
		ff->pFrameRGB->linesize);
	perP = wP->rowbytes / sizeof(PF_Pixel)-wP->width;
	perD = ff->pFrameRGB->linesize[0] - PIXEL_DEPTH_BYTE*wP->width;
	//std::cout << "height=" << wP->height << ",width=" << wP->width << std::endl;
	for (i = 0, pixel = wP->data, data = ff->pFrameRGB->data[0]; i < wP->height; ++i, pixel += perP, data += perD)
	for (j = 0; j < wP->width; ++j, ++pixel, data += PIXEL_DEPTH_BYTE){
		//PF_Pixel *pixel = sampleIntegral32(wP, j, i);
		//std::cout <<"(i,j)=("<< i<<"," << j << std::endl;
		//uint8_t	*data = ff->pFrameRGB->data[0] + (i*ff->pFrameRGB->linesize[0] + j * PIXEL_DEPTH_BYTE);
		pixel->alpha	= data[0];
		pixel->red		= data[1];
		pixel->green	= data[2];
		pixel->blue		= data[3];
	}
	//std::cout << "succeed!" << std::endl;
}

static A_Err	
Logo_DrawSparseFrame(
	AEIO_BasicData					*basic_dataP,
	AEIO_InSpecH					specH, 
	const AEIO_DrawSparseFramePB	*sparse_framePPB, 
	PF_EffectWorld					*wP,
	AEIO_DrawingFlags				*draw_flagsP){
	clock_t t0 = clock();
	A_Err				err			=	A_Err_NONE,
						err2		=	A_Err_NONE;
	AEIO_Handle			optionsH	=	NULL;
	Logo_FileHeader		*headerP	=	NULL;
	ffmpeg_info			*ff;
	int64_t				//seek_target = static_cast<int64_t>(sparse_framePPB->tr.value)*AV_TIME_BASE / sparse_framePPB->tr.scale,
						seek_pts,
						per_pts,
						tolerance;
	AEGP_SuiteHandler	suites(basic_dataP->pica_basicP);
	AVRational			timebaseQ	=	{ 1, AV_TIME_BASE };
	AVPacket			*packet = (AVPacket *)av_malloc(sizeof(AVPacket));
	int					ret, 
						skipB,
						got_picture		=	FALSE,
						seek_picture	=	FALSE;
	av_init_packet(packet);
	//std::cout << "AEGP_GetInSpecOptionsHandle" << std::endl;
	ERR(suites.IOInSuite4()->AEGP_GetInSpecOptionsHandle(specH, reinterpret_cast<void**>(&optionsH)));
	//std::cout << "AEGP_LockMemHandle" << std::endl;
	//ERR(suites.MemorySuite1()->AEGP_LockMemHandle(optionsH, reinterpret_cast<void **>(&headerP)));
	//headerP = reinterpret_cast<Logo_FileHeader*>(optionsH)+0x10;
	//std::cout << "Begin" << std::endl;
	//std::cout << "addr1: " << headerP << std::endl;
	ERR(suites.MemorySuite1()->AEGP_LockMemHandle(optionsH, reinterpret_cast<void **>(&headerP)));
	//std::cout << "addr2: " << headerP << std::endl;
	//headerP = (Logo_FileHeader*)(*optionsH);
	if (!err){
		
		ff = &headerP->ffmpeg;
		//std::cout << "draw path=" << headerP->path << std::endl;
		ff->img_convert_ctx = sws_getContext(	ff->pCodecCtx->width,
												ff->pCodecCtx->height,
												ff->pCodecCtx->pix_fmt,
												wP->width,
												wP->height,
												PIX_FMT_ARGB,
												//PIX_FMT_RGB24,
												SWS_POINT,
												//SWS_BICUBIC, 
												NULL,
												NULL,
												NULL);
		skipB = ff->pCodecCtx->codec_id == AV_CODEC_ID_H264 || ff->pCodecCtx->codec_id == AV_CODEC_ID_H265;
		//std::cout << "skipB=" << skipB << std::endl;
		//seek_target = av_rescale_q(seek_target, timebaseQ, ff->videoStream->time_base);
		seek_pts =	(static_cast<int64_t>(sparse_framePPB->tr.value)*ff->videoStream->time_base.den) /
					(static_cast<int64_t>(sparse_framePPB->tr.scale)*ff->videoStream->time_base.num);
		per_pts =	(ff->videoStream->r_frame_rate.den*ff->videoStream->time_base.den) /
					(ff->videoStream->r_frame_rate.num*ff->videoStream->time_base.num);
		tolerance = per_pts/2;
		seek_pts += ff->videoStream->start_time;
		std::cout << "seek time=" << sparse_framePPB->tr.value << "/" << sparse_framePPB->tr.scale << ";seek_pts=" << seek_pts << std::endl;
		std::cout << "last_pts=" << ff->last_pts << ";per_pts=" << per_pts << std::endl;
		if (ff->last_pts>=0 && seek_pts >= ff->last_pts - tolerance && seek_pts <= ff->last_pts + tolerance)
			seek_picture = TRUE;
		else if (ff->last_pts == AUDIO_CHANGE_PTS || seek_pts < ff->last_pts - tolerance || seek_pts>ff->last_pts + tolerance + CACHE_FRAMES*per_pts){
			avcodec_flush_buffers(ff->pCodecCtx);
			av_seek_frame(ff->pFormatCtx, ff->videoIndex, /*seek_target*/seek_pts, AVSEEK_FLAG_BACKWARD);
			if (skipB)
				ff->pCodecCtx->skip_frame = AVDISCARD_NONREF;
		}
		for (; !seek_picture && av_read_frame(ff->pFormatCtx, packet) >= 0;){

			if (packet->stream_index == ff->videoIndex){
				std::cout << "                packet_pts=" << packet->pts << ";packet_flags=" << packet->flags << ";packet_size=" <<packet->size<< std::endl;
				if (packet->pts >= seek_pts - tolerance)
					ff->pCodecCtx->skip_frame = AVDISCARD_DEFAULT;

				ret = avcodec_decode_video2(ff->pCodecCtx, ff->pFrame, &got_picture, packet);
				if (ret < 0)
//					break;
					std::cout << "ret=" << ret << std::endl;
				if (got_picture){
					//std::cout << "seek_pts=" << seek_pts << ";frame_pkt_pts=" << ff->pFrame->pkt_pts << std::endl;
					std::cout << "frame_pkt=" << ff->pFrame->pkt_pts << ";pict_type=" << ff->pFrame->pict_type << std::endl;
					if (ff->pFrame->pkt_pts >= seek_pts - tolerance)
						seek_picture = TRUE;
				}
			}
			av_free_packet(packet);
		}
		if (!seek_picture/* && !ff->endFix*/){
			ff->pCodecCtx->skip_frame = AVDISCARD_DEFAULT;
			std::cout << "cache!!!!!!!!!!!!!!!!!!!!!" << std::endl;
			//if (!skipB || seek_pts<=ff->last_pframe)
				while (!seek_picture){
					ret = avcodec_decode_video2(ff->pCodecCtx, ff->pFrame, &got_picture, packet);
//					if (ret < 0 || !got_picture)
					if (!got_picture)
						break;
					std::cout << "frame_pkt=" << ff->pFrame->pkt_pts << ";pict_type=" << ff->pFrame->pict_type << std::endl;
					if (ff->pFrame->pkt_pts >= seek_pts - tolerance)
						seek_picture = TRUE;
				}
		}
		if (seek_picture){
			FfmpegToAE(ff, wP);
			std::cout << "SUCCEED!" << std::endl;
		}
		else{
			PF_Pixel spareColor = { PF_MAX_CHAN8, 0, 0, 0 };
			suites.FillMatteSuite2()->fill(0, &spareColor, NULL, wP);
			std::cout << "SPARE!!!" << std::endl;
			//PRINTXX1(0, "SPARE");
		}
		ff->last_pts = seek_pts;
		sws_freeContext(ff->img_convert_ctx);
	}
	av_free(packet);
	ERR2(suites.MemorySuite1()->AEGP_UnlockMemHandle(optionsH));
	std::cout <<"frame time"<< clock() - t0 << std::endl;
	//std::cout << "AEGP_UnlockMemHandle" << std::endl;
	/*
A_Err	err		=	A_Err_NONE;
	AEGP_SuiteHandler	suites(basic_dataP->pica_basicP);
	A_long				time0	=	(A_long)((PF_FpLong)sparse_framePPB->tr.value/sparse_framePPB->tr.scale);
	PF_Pixel			color = { PF_MAX_CHAN8, time0%PF_MAX_CHAN8, time0%PF_MAX_CHAN8, time0%PF_MAX_CHAN8 };
	PF_Rect				rectR	=	{0, 0, 0, 0};
	//	If the sparse_frame required rect is NOT all zeroes,
	//	use it. Otherwise, just blit the whole thing.
	//ERR(suites.MemorySuite1()->AEGP_LockMemHandle(optionsH, reinterpret_cast<void **>(&headerP)));
	if (!(sparse_framePPB->required_region.top	== 0 &&
			sparse_framePPB->required_region.left == 0 &&
			sparse_framePPB->required_region.bottom == 0 &&
			sparse_framePPB->required_region.right == 0)){
		
		rectR.top		= sparse_framePPB->required_region.top;
		rectR.bottom	= sparse_framePPB->required_region.bottom;
		rectR.left		= sparse_framePPB->required_region.left;
		rectR.right		= sparse_framePPB->required_region.right;

		err = suites.FillMatteSuite2()->fill(	0, 
												&color, 
												&rectR,
												wP);
	} else {
		err = suites.FillMatteSuite2()->fill(	0, 
												&color, 
												NULL, // using NULL fills the entire frame
												wP);
	}
	*/
	return err;
};

static A_Err	
Logo_GetDimensions(
	AEIO_BasicData			*basic_dataP,
	AEIO_InSpecH			 specH, 
	const AEIO_RationalScale *rs0,
	A_long					 *width0, 
	A_long					 *height0)
{ 
	return AEIO_Err_USE_DFLT_CALLBACK; 
};
					
static A_Err	
Logo_GetDuration(
	AEIO_BasicData	*basic_dataP,
	AEIO_InSpecH	specH, 
	A_Time			*tr)
{ 
	return AEIO_Err_USE_DFLT_CALLBACK; 
};

static A_Err	
Logo_GetTime(
	AEIO_BasicData	*basic_dataP,
	AEIO_InSpecH	specH, 
	A_Time			*tr)
{ 
	return AEIO_Err_USE_DFLT_CALLBACK; 
};

static A_Err	
Logo_GetSound(
	AEIO_BasicData				*basic_dataP,	
	AEIO_InSpecH				specH,
	AEIO_SndQuality				quality,
	const AEIO_InterruptFuncs	*interrupt_funcsP0,	
	const A_Time				*startPT,	
	const A_Time				*durPT,	
	A_u_long					start_sampLu,
	A_u_long					num_samplesLu,
	void						*dataPV)
{
	std::cout << "GetSound!---Begin" << std::endl;
	std::cout << "num_samplesLu=" << num_samplesLu << std::endl;
	std::cout << "start_sampLu" << start_sampLu << std::endl;
	A_Err				err			=	A_Err_NONE, 
						err2		=	A_Err_NONE;
	AEIO_Handle			optionsH	=	NULL;
	Logo_FileHeader		*headerP	=	NULL;
	ffmpeg_info			*ff			=	NULL;
	int64_t				//seek_target = static_cast<int64_t>(startPT->value)*AV_TIME_BASE / startPT->scale,
						seek_pts,
						end_pts,
						per_pts,
						tolerance;
	AEGP_SuiteHandler	suites(basic_dataP->pica_basicP);
	AVRational			timebaseQ	=	{ 1, AV_TIME_BASE };
	AVPacket			*packet = (AVPacket *)av_malloc(sizeof(AVPacket));
	int					got_picture, 
						ret,
						seek_audio = FALSE;
	char				*destP = (char*)dataPV,
						*endAddre;
	struct SwrContext	*au_convert_ctx;

	av_init_packet(packet);



	ERR(suites.IOInSuite4()->AEGP_GetInSpecOptionsHandle(specH, reinterpret_cast<void**>(&optionsH)));
	ERR(suites.MemorySuite1()->AEGP_LockMemHandle(optionsH, reinterpret_cast<void**>(&headerP)));

	if (!err && headerP) {
		ff = &headerP->ffmpeg;
		endAddre = num_samplesLu * headerP->num_channels*headerP->bytes_per_sample + (char*)dataPV;
		//seek_target =	av_rescale_q(seek_target, timebaseQ, ff->audioStream->time_base);
		seek_pts	=	ff->audioStream->start_time;
		seek_pts	+=	(static_cast<int64_t>(startPT->value)*ff->audioStream->time_base.den) /
						(static_cast<int64_t>(startPT->scale)*ff->audioStream->time_base.num);
		end_pts		=	seek_pts +
						(static_cast<int64_t>(durPT->value)*ff->audioStream->time_base.den) /
						(static_cast<int64_t>(durPT->scale)*ff->audioStream->time_base.num);
		tolerance	=	2;
		//std::cout << "seek_pts=" << seek_pts << ";end_pts=" << end_pts << ";per_pts=" << per_pts << std::endl;
		uint64_t out_channel_layout = AV_CH_LAYOUT_STEREO;
		int out_nb_samples = 1024;
		int out_channels = av_get_channel_layout_nb_channels(out_channel_layout);
		AVSampleFormat out_sample_fmt = AV_SAMPLE_FMT_FLT;
		int out_buffer_size = av_samples_get_buffer_size(NULL, out_channels, out_nb_samples, out_sample_fmt, 1);

		uint8_t *out_buffer = (uint8_t *)av_malloc(MAX_AUDIO_FRAME_SIZE * 2);

		std::cout << "aCodecCtx sample_fmt=" << ff->aCodecCtx->sample_fmt << std::endl;
		au_convert_ctx = swr_alloc();
		au_convert_ctx = swr_alloc_set_opts(au_convert_ctx, 
											out_channel_layout,
											out_sample_fmt,
											ff->aCodecCtx->sample_rate,
											ff->aCodecCtx->channel_layout,
											ff->aCodecCtx->sample_fmt, 
											ff->aCodecCtx->sample_rate, 
											0, 
											NULL);


		swr_init(au_convert_ctx);

		avcodec_flush_buffers(ff->aCodecCtx);
		av_seek_frame(ff->pFormatCtx, ff->audioIndex, /*seek_target*/seek_pts, AVSEEK_FLAG_BACKWARD);
		ff->last_pts = AUDIO_CHANGE_PTS;
		while (!seek_audio && av_read_frame(ff->pFormatCtx, packet) >= 0){
			if (packet->stream_index == ff->audioIndex){
				//std::cout << "          packet_pts=" << packet->pts << std::endl;
				ret = avcodec_decode_audio4(ff->aCodecCtx, ff->aFrame, &got_picture, packet);
				if (ret < 0)
					break;
				if (got_picture > 0){
					//std::cout << "frame format=" << ff->aFrame->format << std::endl;
					//std::cout <<"audio_pts="<< ff->aFrame->pkt_pts << std::endl;
					if (ff->aFrame->pkt_pts >= seek_pts - tolerance){
						int ret2 = swr_convert(au_convert_ctx, &out_buffer, MAX_AUDIO_FRAME_SIZE, (const uint8_t **)ff->aFrame->data, ff->aFrame->nb_samples);
						int outsize = ret2*out_channels * av_get_bytes_per_sample(out_sample_fmt);
					//	std::cout << "outsize=" << outsize << std::endl;
						outsize = MIN(outsize, endAddre - destP);
						//if (destP + outsize <= endAddre){
							memcpy(destP, out_buffer, outsize);
							destP = (char*)destP + outsize;
						//}
						seek_audio = TRUE;
					}
				}
			}
			av_free_packet(packet);
		}
		std::cout << "rending!" << std::endl;
		seek_audio = FALSE;

		while (!seek_audio && av_read_frame(ff->pFormatCtx, packet) >= 0){
			if (packet->stream_index == ff->audioIndex){
				//std::cout << "          packet_pts=" << packet->pts << std::endl;


				ret = avcodec_decode_audio4(ff->aCodecCtx, ff->aFrame, &got_picture, packet);
				if (ret < 0)
					break;
				if (got_picture > 0){
					//std::cout << "audio_pts=" << ff->aFrame->pkt_pts << std::endl;
					//if (ff->aFrame->pkt_pts >= end_pts - tolerance)
					if (destP == endAddre)
						seek_audio = TRUE;
					else{
						int ret2 = swr_convert(au_convert_ctx, &out_buffer, MAX_AUDIO_FRAME_SIZE, (const uint8_t **)ff->aFrame->data, ff->aFrame->nb_samples);
						int outsize = ret2*out_channels * av_get_bytes_per_sample(out_sample_fmt);
					//	std::cout << "outsize=" << outsize << std::endl;
						outsize = MIN(outsize, endAddre - destP);
						
						//if (destP + outsize <= endAddre){
							memcpy(destP, out_buffer, outsize);
							destP = (char*)destP + outsize;
						//}
					}
				}
			}
			av_free_packet(packet);
		}
		av_free(out_buffer);
		swr_free(&au_convert_ctx);
		//std::cout << "seek_audio=" << seek_audio << std::endl;
	}
	//std::cout << "end rending"<< std::endl;
	av_free(packet);
	
	ERR2(suites.MemorySuite1()->AEGP_UnlockMemHandle(optionsH));
	memset(destP, 0, endAddre - destP);
	std::cout << "addr offset" << (int)((char*)destP - dataPV) << std::endl;
	//std::cout << "GetSound!---End" << std::endl;
	return err;
};

static A_Err	
Logo_InqNextFrameTime(
	AEIO_BasicData			*basic_dataP,
	AEIO_InSpecH			specH, 
	const A_Time			*base_time_tr,
	AEIO_TimeDir			time_dir, 
	A_Boolean				*found0,
	A_Time					*key_time_tr0)
{ 
	return AEIO_Err_USE_DFLT_CALLBACK; 
};


static A_Err	
Logo_CloseSourceFiles(
	AEIO_BasicData	*basic_dataP,
	AEIO_InSpecH			seqH)
{ 
	return A_Err_NONE; 
};		// TRUE for close, FALSE for unclose

static A_Err	
Logo_CountUserData(
	AEIO_BasicData	*basic_dataP,
	AEIO_InSpecH			inH,
	A_u_long 				typeLu,
	A_u_long				max_sizeLu,
	A_u_long				*num_of_typePLu)
{ 
	return A_Err_NONE; 
};
static A_Err
Logo_InflateOptions(
	AEIO_BasicData	*basic_dataP,
	AEIO_InSpecH	specH,
	AEIO_Handle		flat_optionsH)
{ 

	//MessageBox(0, "Logo_InflateOptions", "Begin", 0);
	std::cout << "------------Logo_InflateOptions Begin" << std::endl;
	A_Err	err		=	A_Err_NONE,
			err2	=	A_Err_NONE; 
	
	AEIO_Handle			optionsH		= NULL,
						old_optionsH	= NULL;
	
	Logo_FlatHeader		*flat_headerP = NULL;

	Logo_FileHeader		*headerP	= NULL;

	AEGP_SuiteHandler	suites(basic_dataP->pica_basicP);
	if (flat_optionsH)
		ERR(suites.MemorySuite1()->AEGP_LockMemHandle(flat_optionsH, reinterpret_cast<void**>(&flat_headerP)));
	if (flat_headerP)
		ERR(suites.MemorySuite1()->AEGP_NewMemHandle(	S_mem_id,
														"inflate_options", 
														sizeof(Logo_FileHeader),
														AEGP_MemFlag_CLEAR, 
														&optionsH));
	if (optionsH)
		ERR(suites.MemorySuite1()->AEGP_LockMemHandle(optionsH, (void**)&headerP));
	if (!err && headerP){
		headerP->path = flat_headerP->path;
		std::cout << "path=" << flat_headerP->path << std::endl;
		ERR(ReadFileHeader(basic_dataP, headerP, flat_headerP->path));
		suites.IOInSuite4()->AEGP_SetInSpecOptionsHandle(specH, optionsH, reinterpret_cast<void**>(&old_optionsH));
	}
	if (optionsH)
		ERR2(suites.MemorySuite1()->AEGP_UnlockMemHandle(optionsH));
	if (flat_optionsH)
		ERR2(suites.MemorySuite1()->AEGP_UnlockMemHandle(flat_optionsH));
	/*	During this function, 
	
		+	Create a non-flat options structure, containing the info from the 
			flat_optionsH you're passed.
		
		+	Set the options for the InSpecH using AEGP_SetInSpecOptionsHandle().
	*/	

	//MessageBox(0, "Logo_InflateOptions", "End", 0);
	std::cout << "------------Logo_InflateOptions End" << std::endl;
	return err; 
};		
static A_Err
Logo_FlattenOptions(
	AEIO_BasicData	*basic_dataP,
	AEIO_InSpecH	specH,
	AEIO_Handle		*flat_optionsPH)
{ 
	//MessageBox(0, "Logo_FlattenOptions", "Begin", 0);
	std::cout << "************Logo_FlattenOptions Begin" << std::endl;
	A_Err	err		=	A_Err_NONE,
			err2	=	A_Err_NONE; 
	AEIO_Handle			optionsH		= NULL;
	
	Logo_FileHeader		*old_headerP	= NULL;
	Logo_FlatHeader		*flat_headerP	= NULL;

	AEGP_SuiteHandler	suites(basic_dataP->pica_basicP);	
	
	//	Given the AEIO_InSpecH, acquire the non-flat
	//	options handle and use it to create a flat
	//	version. Do NOT de-allocate the non-flat
	//	options handle!
	
	ERR(suites.IOInSuite4()->AEGP_GetInSpecOptionsHandle(specH, reinterpret_cast<void**>(&optionsH)));

	if (optionsH) 
		ERR(suites.MemorySuite1()->AEGP_LockMemHandle(optionsH, reinterpret_cast<void**>(&old_headerP)));
			
	if (old_headerP)
		ERR(suites.MemorySuite1()->AEGP_NewMemHandle(	S_mem_id,
														"flattened_options", 
														sizeof(Logo_FlatHeader),
														AEGP_MemFlag_CLEAR, 
														flat_optionsPH));
	if (*flat_optionsPH)
		ERR(suites.MemorySuite1()->AEGP_LockMemHandle(*flat_optionsPH, (void**)&flat_headerP));
	if (!err && flat_headerP) {
		strcpy(flat_headerP->path, old_headerP->path.c_str());
		std::cout << "path=" << flat_headerP->path << std::endl;
		// Here is where you should provide a disk-safe copy of your options data
		//ERR(PretendToReadFileHeader(basic_dataP, flat_headerP, old_headerP->path.data()));
	}

	if (optionsH)
	{
		ERR2(suites.MemorySuite1()->AEGP_UnlockMemHandle(optionsH));
	}
	if (*flat_optionsPH)
	{
		ERR2(suites.MemorySuite1()->AEGP_UnlockMemHandle(*flat_optionsPH));
	}
	std::cout << "err=" << err << std::endl;

	//MessageBox(0, "Logo_FlattenOptions", "End", 0);
	std::cout << "************Logo_FlattenOptions End" << std::endl;
	return err;
};		

static A_Err	
Logo_GetUserData(   
	AEIO_BasicData	*basic_dataP,
	AEIO_InSpecH			inH,
	A_u_long 				typeLu,
	A_u_long				indexLu,
	A_u_long				max_sizeLu,
	AEIO_Handle				*dataPH)
{ 
	return A_Err_NONE; 
};
static A_Err
Logo_SynchInSpec(
	AEIO_BasicData	*basic_dataP,
	AEIO_InSpecH	specH, 
	A_Boolean		*changed0)
{ 
	return AEIO_Err_USE_DFLT_CALLBACK;
};

static A_Err	
Logo_VerifyFileImportable(
	AEIO_BasicData			*basic_dataP,
	AEIO_ModuleSignature	sig, 
	const A_UTF16Char		*file_pathZ, 
	A_Boolean				*importablePB)
{ 
	//	This function is an appropriate place to do
	//	in-depth evaluation of whether or not a given
	//	file will be importable; AE already does basic
	//	extension checking. Keep in mind that this 
	//	function should be fairly speedy, to keep from
	//	bogging down the user while selecting files.
	
	//	-bbb 3/31/04

	*importablePB = TRUE;
	return A_Err_NONE; 
};		


static A_Err
ConstructModuleInfo(
	SPBasicSuite		*pica_basicP,			
	AEIO_ModuleInfo		*info)
{
	A_Err err = A_Err_NONE;
	AEGP_SuiteHandler	suites(pica_basicP);
	
	if (info) {
		
		info->sig						=	'EPT_';
		info->max_width					=	1920;
		info->max_height					=	1080;
		info->num_filetypes				=	1;
		info->num_extensions				=	4;
		info->num_clips					=	0;
		/*
		info->create_kind.type			=	'IPT_';
		info->create_kind.creator		=	AEIO_ANY_CREATOR;

		info->create_ext.pad			=	'.';
		info->create_ext.extension[0]	=	'i';
		info->create_ext.extension[1]	=	'p';
		info->create_ext.extension[2]	=	't';

		
		
		info->num_aux_extensionsS		=	0;
		*/
		suites.ANSICallbacksSuite1()->strcpy(info->name, "Logo_Importer");
		info->flags						=	AEIO_MFlag_INPUT	|
										//	AEIO_MFlag_NO_TIME	|
											AEIO_MFlag_VIDEO	|
											AEIO_MFlag_AUDIO	|
											AEIO_MFlag_FILE;

		info->read_kinds[0].mac.type				=	'EPT_';
		info->read_kinds[0].mac.creator			=	AEIO_ANY_CREATOR;
		info->read_kinds[1].ext.pad				=	'.';
		info->read_kinds[1].ext.extension[0]		=	'e';
		info->read_kinds[1].ext.extension[1]		=	'p';
		info->read_kinds[1].ext.extension[2]		=	't';
		info->read_kinds[2].ext.pad				=	'.';
		info->read_kinds[2].ext.extension[0]		=	'm';
		info->read_kinds[2].ext.extension[1]		=	'k';
		info->read_kinds[2].ext.extension[2]		=	'v';
		info->read_kinds[3].ext.pad				=	'.';
		info->read_kinds[3].ext.extension[0]		=	'f';
		info->read_kinds[3].ext.extension[1]		=	'l';
		info->read_kinds[3].ext.extension[2]		=	'v';
		info->read_kinds[4].ext.pad				=	'.';
		info->read_kinds[4].ext.extension[0]		=	'm';
		info->read_kinds[4].ext.extension[1]		=	'a';
		info->read_kinds[4].ext.extension[2]		=	'd';

	} else {
		err = A_Err_STRUCT;
	}
	return err;
}

A_Err
ConstructFunctionBlock(
	AEIO_FunctionBlock4	*funcs)
{
	freopen("Plug-ins\\logo_plus\\logo_importer_dialog.txt", "w", stdout);
	if (funcs) {
		//funcs->AEIO_AddFrame				=	Logo_AddFrame;
		//funcs->AEIO_AddMarker				=	Logo_AddMarker;
		//funcs->AEIO_AddSoundChunk			=	Logo_AddSoundChunk;
		funcs->AEIO_CloseSourceFiles			=	Logo_CloseSourceFiles;
		funcs->AEIO_CountUserData			=	Logo_CountUserData;
		funcs->AEIO_DisposeInSpec			=	Logo_DisposeInSpec;
		//funcs->AEIO_DisposeOutputOptions	=	Logo_DisposeOutputOptions;
		funcs->AEIO_DrawSparseFrame			=	Logo_DrawSparseFrame;
		//funcs->AEIO_EndAdding				=	Logo_EndAdding;
		funcs->AEIO_FlattenOptions			=	Logo_FlattenOptions;
		//funcs->AEIO_Flush					=	Logo_Flush;
		funcs->AEIO_GetActiveExtent			=	Logo_GetActiveExtent;
		//funcs->AEIO_GetDepths				=	Logo_GetDepths;
		funcs->AEIO_GetDimensions			=	Logo_GetDimensions;
		funcs->AEIO_GetDuration				=	Logo_GetDuration;
		funcs->AEIO_GetInSpecInfo			=	Logo_GetInSpecInfo;
		//funcs->AEIO_GetOutputInfo			=	Logo_GetOutputInfo;
		//funcs->AEIO_GetOutputSuffix		=	Logo_GetOutputSuffix;
		//funcs->AEIO_GetSizes				=	Logo_GetSizes;
		funcs->AEIO_GetSound					=	Logo_GetSound;
		funcs->AEIO_GetTime					=	Logo_GetTime;
		funcs->AEIO_GetUserData				=	Logo_GetUserData;
		//funcs->AEIO_Idle					=	Logo_Idle;
		funcs->AEIO_InflateOptions			=	Logo_InflateOptions;
		funcs->AEIO_InitInSpecFromFile		=	Logo_InitInSpecFromFile;
		funcs->AEIO_InitInSpecInteractive	=	Logo_InitInSpecInteractive;
		funcs->AEIO_InqNextFrameTime			=	Logo_InqNextFrameTime;
		//funcs->AEIO_NumAuxFiles			=	Logo_NumAuxFiles;
		//funcs->AEIO_OutputFrame			=	Logo_OutputFrame;
		//funcs->AEIO_SeqOptionsDlg			=	Logo_SeqOptionsDlg;
		//funcs->AEIO_SetOutputFile			=	Logo_SetOutputFile;
		//funcs->AEIO_SetUserData			=	Logo_SetUserData;
		//funcs->AEIO_StartAdding			=	Logo_StartAdding;
		funcs->AEIO_SynchInSpec				=	Logo_SynchInSpec;
		//funcs->AEIO_UserOptionsDialog		=	Logo_UserOptionsDialog;
		funcs->AEIO_VerifyFileImportable		=	Logo_VerifyFileImportable;
		//funcs->AEIO_WriteLabels			=	Logo_WriteLabels;
		//funcs->AEIO_InitOutputSpec			=	Logo_InitOutputSpec;
		//funcs->AEIO_GetFlatOutputOptions	=	Logo_GetFlatOutputOptions;
		//funcs->AEIO_OutputInfoChanged		=	Logo_OutputInfoChanged;

		return A_Err_NONE;
	} else {
		return A_Err_STRUCT;
	}
}
A_Err
EntryPointFunc(
	struct SPBasicSuite		*pica_basicP,			/* >> */
	A_long				 	major_versionL,			/* >> */		
	A_long					minor_versionL,			/* >> */		
	AEGP_PluginID			aegp_plugin_id,			/* >> */
	AEGP_GlobalRefcon		*global_refconP)		/* << */
{
	A_Err 				err					= A_Err_NONE;
	AEIO_ModuleInfo		info;
	AEIO_FunctionBlock4	funcs;
	AEGP_SuiteHandler	suites(pica_basicP);
	av_register_all();
	AEFX_CLR_STRUCT(info);
	AEFX_CLR_STRUCT(funcs);
	
	ERR(suites.RegisterSuite5()->AEGP_RegisterDeathHook(aegp_plugin_id, DeathHook, 0));
	ERR(ConstructModuleInfo(pica_basicP, &info));
	ERR(ConstructFunctionBlock(&funcs));

	ERR(suites.RegisterSuite5()->AEGP_RegisterIO(	aegp_plugin_id,
													0,
													&info, 
													&funcs));

	ERR(suites.UtilitySuite3()->AEGP_RegisterWithAEGP(	NULL,
														"Logo_Importer",
														&S_mem_id));
	return err;
}
