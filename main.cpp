#define _CRT_SECURE_NO_WARNINGS
#include <assert.h>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
};

#ifdef NDEBUG
#pragma comment(lib, "ffmpeg/lib/swscale.lib")
#pragma comment(lib, "ffmpeg/lib/avutil.lib")
#pragma comment(lib, "ffmpeg/lib/avformat.lib")
#pragma comment(lib, "ffmpeg/lib/avcodec.lib")
#else
#pragma comment(lib, "ffmpeg/lib/swscale.lib")
#pragma comment(lib, "../ffmpeg/ffmpeg-20150831-git-1acd631/libavutil/avutil.lib")
#pragma comment(lib, "../ffmpeg/ffmpeg-20150831-git-1acd631/libavformat/avformat.lib")
#pragma comment(lib, "../ffmpeg/ffmpeg-20150831-git-1acd631/libavcodec/avcodec.lib")
#endif

#define OUT_WIDTH 1280
#define OUT_HEIGHT 720
#define OUT_BITRATE 3500000

#ifdef WIN32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

struct Mutex
{
	Mutex() { h = CreateMutexA(NULL, FALSE, NULL); }
	~Mutex() { CloseHandle(h); };
	inline bool Lock() { return WaitForSingleObject(h, INFINITE) == WAIT_OBJECT_0; }
	inline void Unlock() { ReleaseMutex(h); }
	private: HANDLE h;
};

struct Semaphore
{
	Semaphore(int startCount = 0) { h = CreateSemaphoreA(NULL, startCount, 32 * 1024, NULL); }
	~Semaphore() { CloseHandle(h); };
	inline bool Wait() { return WaitForSingleObject(h, INFINITE) == WAIT_OBJECT_0; }
	inline bool TryWait() { return WaitForSingleObject(h, 0) == WAIT_OBJECT_0; }
	inline void Post() { ReleaseSemaphore(h, 1, NULL); }
	private: HANDLE h;
};

struct Thread
{
	typedef int (*FuncPtr)(const void* p);
	Thread(FuncPtr pFunc, const void* p = NULL) : pFunc(pFunc), p(p), h(CreateThread(0, 0, START_ROUTINE, this, 0, NULL)) { }
	bool Wait() { if (!pFunc) return false; pFunc = NULL; return WaitForSingleObject(h, INFINITE) == WAIT_OBJECT_0; }
	bool HasEnded() { return WaitForSingleObject(h, INFINITE) == WAIT_OBJECT_0; }
	~Thread() { Wait(); CloseHandle(h); } 
	private: HANDLE h; const void* p; FuncPtr pFunc; 
	static DWORD WINAPI START_ROUTINE(LPVOID p) { Thread* self = (Thread*)p; return (DWORD)(*self->pFunc)(self->p); }
};

inline uint32_t GetTicksU32() { return GetTickCount(); }

#else
#error NEEDS PLATFORM SUPPORT
#endif

static Mutex mtxFramesToEncode;
static Semaphore semFramesToEncode, semFramesEncoded;
static std::vector< std::pair<AVFrame*, AVMediaType> > framesToEncode;

static SwsContext *sws_imgdet_ctx = NULL, *sws_outvid_ctx = NULL;
static uint8_t *sws_imgdet_data[4], *sws_imgdet_prev_data[4];
static int sws_imgdet_linesize[4];
static AVFrame* sws_outvid_frame = NULL;

struct sFrameBuffering
{
	struct sStream
	{
		int64_t count;
		struct sBackupFrame { AVFrame* frame; int64_t ptsStream; };
		std::vector<sBackupFrame> backup_frames;
		sStream() : count(0) { }
	} vid, aud;
	int64_t ptsMovingDetectionOutOfRange, ptsMovingStarted, ptsMovingEnded, ptsAudioAmountVideoFrames, ptsAudioSamplesWritten, ptsDebugVideoFramesActuallyWritten;
	sFrameBuffering() : ptsMovingDetectionOutOfRange(-1), ptsMovingStarted(-1), ptsMovingEnded(-1), ptsAudioAmountVideoFrames(0), ptsAudioSamplesWritten(0), ptsDebugVideoFramesActuallyWritten(0) { }
};

struct sStreamStatus
{
	AVFormatContext *fmt_ctx;
	AVStream *video, *audio;
	sStreamStatus() : fmt_ctx(NULL), video(NULL), audio(NULL) { }
};

static AVRational TimeBaseInputStream;
static AVCodecContext InputVideoCodecCtx, InputAudioCodecCtx, OutputVideoCodecCtx, OutputAudioCodecCtx;
static bool packetReadingInputIsValid = true;

static FILE* fLogFile = NULL;
static Mutex fLogFileMutex;
static void logcallback(void *avcl, int level, const char *fmt, va_list vl)
{
	if (level >= AV_LOG_VERBOSE) return;
	if (level != -999) av_log_default_callback(avcl, level, fmt, vl);
	if (fLogFile) { fLogFileMutex.Lock(); vfprintf(fLogFile, fmt, vl); fflush(fLogFile); fLogFileMutex.Unlock(); }
}
static void show(const char *fmt, ...) { va_list vl; va_start(vl, fmt); logcallback(NULL, AV_LOG_WARNING, fmt, vl); va_end(vl); }
static void showlogonly(const char *fmt, ...) { va_list vl; va_start(vl, fmt); logcallback(NULL, -999, fmt, vl); va_end(vl); }

static int read_packet(void *opaque, uint8_t *buf, int buf_size)
{
#ifdef NDEBUG
	DWORD read;
	if (!ReadFile(GetStdHandle(STD_INPUT_HANDLE), buf, buf_size, &read, NULL)) { packetReadingInputIsValid = false; return 0; }
	return (int)read;
#else
	static const char* fTestFileName = "test.vod.ak.hls.ttvnw.net.mp4";
	if (!packetReadingInputIsValid) return 0;
	static FILE* fTest = NULL;
	if (fTest == NULL) { show("DEBUG BUILD: USING TEST FILE [%s] INSTEAD OF STDIN\n\n", fTestFileName); if (!(fTest = fopen(fTestFileName, "rb"))) { show("TEST FILE NOT FOUND\nABORTING\n\n"); exit(1); } }
	size_t read_Input = fread(buf, 1, buf_size, fTest);
	if (read_Input != buf_size) { packetReadingInputIsValid = false; fseek(fTest, 0, SEEK_SET); }
	return (int)read_Input;
#endif
}

static bool OpenInput(sStreamStatus& sti)
{
	const int avio_ctx_buffer_size = 4096;
	if (!(sti.fmt_ctx = avformat_alloc_context())) return false;
	if (!(sti.fmt_ctx->pb = avio_alloc_context((uint8_t*)av_malloc(avio_ctx_buffer_size), avio_ctx_buffer_size, 0, NULL, &read_packet, NULL, NULL))) return false;
	if (avformat_open_input(&sti.fmt_ctx, NULL, NULL, NULL) < 0) return false;
	if (avformat_find_stream_info(sti.fmt_ctx, NULL) < 0) { fprintf(stderr, "Could not find stream information\n"); return false; }

	AVCodec *video_dec;
	int i_video_stream_idx = av_find_best_stream(sti.fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, &video_dec, 0);
	if (i_video_stream_idx < 0) { av_log(NULL, AV_LOG_ERROR, "Cannot find a video stream in the input file\n"); return false; }
	if (!video_dec) { av_log(NULL, AV_LOG_ERROR, "Cannot find a decoder for the selected video stream\n"); return false; }
	sti.video = sti.fmt_ctx->streams[i_video_stream_idx];
	av_opt_set_int(sti.video->codec, "refcounted_frames", 1, 0);
	if (avcodec_open2(sti.video->codec, video_dec, NULL) < 0) { av_log(NULL, AV_LOG_ERROR, "Cannot open video decoder\n"); return false; }

	AVCodec *audio_dec;
	int i_audio_stream_idx = av_find_best_stream(sti.fmt_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, &audio_dec, 0);
	if (i_audio_stream_idx < 0) { av_log(NULL, AV_LOG_ERROR, "Cannot find a audio stream in the input file\n"); return false; }
	if (!audio_dec) { av_log(NULL, AV_LOG_ERROR, "Cannot find a decoder for the selected audio stream\n"); return false; }
	sti.audio = sti.fmt_ctx->streams[i_audio_stream_idx];
	av_opt_set_int(sti.audio->codec, "refcounted_frames", 1, 0);
	if (avcodec_open2(sti.audio->codec, audio_dec, NULL) < 0) { av_log(NULL, AV_LOG_ERROR, "Cannot open audio decoder\n"); return false; }

	av_dump_format(sti.fmt_ctx, 0, "stream", 0);
	return true;
}

static void CloseInput(sStreamStatus& sti)
{
	avcodec_close(sti.video->codec);
	avcodec_close(sti.audio->codec);
	av_freep(&sti.fmt_ctx->pb->buffer);
	av_freep(&sti.fmt_ctx->pb);
	avformat_close_input(&sti.fmt_ctx);
	sti.video = sti.audio = NULL;
}

static bool OpenOutput(sStreamStatus& sto, const char* ofilename)
{
	avformat_alloc_output_context2(&sto.fmt_ctx, NULL, NULL, ofilename);
	if (!sto.fmt_ctx) { av_log(NULL, AV_LOG_ERROR, "Could not create output context\n"); return false; }

	AVCodec *video_enc = avcodec_find_encoder(InputVideoCodecCtx.codec_id);
	if (!video_enc) { av_log(NULL, AV_LOG_FATAL, "Necessary encoder not found\n"); return false; }
	sto.video = avformat_new_stream(sto.fmt_ctx, NULL);
	if (!sto.video) { av_log(NULL, AV_LOG_ERROR, "Failed allocating output stream\n"); return false; }
	sto.video->time_base = TimeBaseInputStream; //will get overridden by avformat_write_header but libav complains without this
	sto.video->codec->width               = OUT_WIDTH; //InputVideoCodecCtx.width;
	sto.video->codec->height              = OUT_HEIGHT; //InputVideoCodecCtx.height;
	sto.video->codec->sample_aspect_ratio = InputVideoCodecCtx.sample_aspect_ratio;
	sto.video->codec->pix_fmt             = InputVideoCodecCtx.pix_fmt;
	sto.video->codec->time_base           = InputVideoCodecCtx.time_base;
	sto.video->codec->ticks_per_frame     = InputVideoCodecCtx.ticks_per_frame;
	sto.video->codec->me_range            = 16;
	sto.video->codec->max_qdiff           = 4;
	sto.video->codec->qmin                = 0;
	sto.video->codec->qmax                = 69;
	sto.video->codec->qcompress           = 0.6f;
    sto.video->codec->profile             = FF_PROFILE_H264_HIGH;
	sto.video->codec->bit_rate            = OUT_BITRATE;
	if (avcodec_open2(sto.video->codec, video_enc, NULL) < 0) { av_log(NULL, AV_LOG_ERROR, "Cannot open video encoder for video stream\n"); return false; }
	if (sto.video->codec->pix_fmt != InputVideoCodecCtx.pix_fmt) { av_log(NULL, AV_LOG_ERROR, "Input and output pixel format must be the same\n"); return false; }
	if (sto.fmt_ctx->oformat->flags & AVFMT_GLOBALHEADER) sto.video->codec->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

	AVCodec *audio_enc = avcodec_find_encoder_by_name("aac"); //using libav built in aac encoder
	if (!audio_enc) { av_log(NULL, AV_LOG_FATAL, "Necessary encoder not found\n"); return false; }
	sto.audio = avformat_new_stream(sto.fmt_ctx, NULL);
	if (!sto.audio) { av_log(NULL, AV_LOG_ERROR, "Failed allocating output stream\n"); return false; }
	sto.audio->time_base = TimeBaseInputStream; //will get overridden by avformat_write_header but libav complains without this
	sto.audio->codec->strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL; //for libav built in aac encoder
	sto.audio->codec->sample_rate           = InputAudioCodecCtx.sample_rate;
	sto.audio->codec->channel_layout        = InputAudioCodecCtx.channel_layout;
	sto.audio->codec->channels              = av_get_channel_layout_nb_channels(sto.audio->codec->channel_layout);
	sto.audio->codec->time_base             = InputAudioCodecCtx.time_base; //{ 1, InputAudioCodecCtx.sample_rate };
	sto.audio->codec->sample_fmt            = audio_enc->sample_fmts[0]; //take first format from list of supported formats
	if (avcodec_open2(sto.audio->codec, audio_enc, NULL) < 0) { av_log(NULL, AV_LOG_ERROR, "Cannot open audio encoder for audio stream\n"); return false; }
	if (sto.audio->codec->sample_fmt != InputAudioCodecCtx.sample_fmt) { av_log(NULL, AV_LOG_ERROR, "Input and output sample format must be the same\n"); return false; }
	if (sto.fmt_ctx->oformat->flags & AVFMT_GLOBALHEADER) sto.audio->codec->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

	if (av_cmp_q(sto.video->codec->time_base, InputVideoCodecCtx.time_base)) { av_log(NULL, AV_LOG_ERROR, "Output video time_base is different from input!\n"); return false; }
	if (av_cmp_q(sto.audio->codec->time_base, InputAudioCodecCtx.time_base)) { av_log(NULL, AV_LOG_ERROR, "Output audio time_base is different from input!\n"); return false; }

	if ((!(sto.fmt_ctx->oformat->flags & AVFMT_NOFILE)) && avio_open(&sto.fmt_ctx->pb, ofilename, AVIO_FLAG_WRITE) < 0) { av_log(NULL, AV_LOG_ERROR, "Could not open output file '%s'", ofilename); return false; }

	if (avformat_write_header(sto.fmt_ctx, NULL) < 0) { av_log(NULL, AV_LOG_ERROR, "Error occurred when opening output file\n"); return false; }

	av_dump_format(sto.fmt_ctx, 0, ofilename, 1);
	return true;
}

static void CloseOutput(sStreamStatus& sto)
{
	av_write_trailer(sto.fmt_ctx);
	avcodec_close(sto.video->codec);
	avcodec_close(sto.audio->codec);
	avformat_close_input(&sto.fmt_ctx);
	sto.video = sto.audio = NULL;
}

static bool SetupSWS()
{
	if (!(sws_imgdet_ctx = sws_getContext(InputVideoCodecCtx.width, InputVideoCodecCtx.height, InputVideoCodecCtx.pix_fmt, 64, 64, AV_PIX_FMT_GRAY8, SWS_BILINEAR, NULL, NULL, NULL)))
		{ fprintf(stderr, "Impossible to create scale context for the conversion fmt:%s s:%dx%d -> fmt:%s s:%dx%d\n", av_get_pix_fmt_name(InputVideoCodecCtx.pix_fmt), InputVideoCodecCtx.width, InputVideoCodecCtx.height, av_get_pix_fmt_name(AV_PIX_FMT_GRAY8), 64, 64); return false; }
	
	if (av_image_alloc(sws_imgdet_data, sws_imgdet_linesize, 64, 64, AV_PIX_FMT_GRAY8, 1) < 0) 
		{ fprintf(stderr, "Could not allocate imgdet image\n"); return false; }

	if (av_image_alloc(sws_imgdet_prev_data, sws_imgdet_linesize, 64, 64, AV_PIX_FMT_GRAY8, 1) < 0) 
		{ fprintf(stderr, "Could not allocate imgdet_prev image\n"); return false; }

	if (InputVideoCodecCtx.width != OUT_WIDTH || InputVideoCodecCtx.height != OUT_HEIGHT)
	{
		show("Opening output scaler for the conversion fmt:%s s:%dx%d -> fmt:%s s:%dx%d\n", av_get_pix_fmt_name(InputVideoCodecCtx.pix_fmt), InputVideoCodecCtx.width, InputVideoCodecCtx.height, av_get_pix_fmt_name(InputVideoCodecCtx.pix_fmt), OUT_WIDTH, OUT_HEIGHT);
		if (!(sws_outvid_ctx = sws_getContext(InputVideoCodecCtx.width, InputVideoCodecCtx.height, InputVideoCodecCtx.pix_fmt, OUT_WIDTH, OUT_HEIGHT, InputVideoCodecCtx.pix_fmt, SWS_BILINEAR, NULL, NULL, NULL)))
			{ fprintf(stderr, "Impossible to create scale context for the conversion fmt:%s s:%dx%d -> fmt:%s s:%dx%d\n", av_get_pix_fmt_name(InputVideoCodecCtx.pix_fmt), InputVideoCodecCtx.width, InputVideoCodecCtx.height, av_get_pix_fmt_name(InputVideoCodecCtx.pix_fmt), OUT_WIDTH, OUT_HEIGHT); return false; }
	}
	else
		show("No output scaler needed for the input fmt:%s s:%dx%d\n", av_get_pix_fmt_name(InputVideoCodecCtx.pix_fmt), InputVideoCodecCtx.width, InputVideoCodecCtx.height);

	sws_outvid_frame = av_frame_alloc();
	sws_outvid_frame->format = InputVideoCodecCtx.pix_fmt;
	sws_outvid_frame->width = OUT_WIDTH;
	sws_outvid_frame->height = OUT_HEIGHT;
	av_frame_get_buffer(sws_outvid_frame, 32);
	return true;
}

static void FreeSWS()
{
	av_frame_free(&sws_outvid_frame);
	if (sws_outvid_ctx) sws_freeContext(sws_outvid_ctx);
	av_freep(&sws_imgdet_prev_data[0]);
	av_freep(&sws_imgdet_data[0]);
	sws_freeContext(sws_imgdet_ctx);
}

static int EncodeWriteFrame(AVFormatContext* ofmt_ctx, AVFrame *frame, const AVStream* o_stream, int *got_frame = NULL)
{
	int got_frame_local; if (!got_frame) got_frame = &got_frame_local;

	//encode frame
	AVPacket enc_pkt;
	enc_pkt.data = NULL;
	enc_pkt.size = 0;
	av_init_packet(&enc_pkt);
	if (frame)
	{
		if (o_stream->codec->codec_type == AVMEDIA_TYPE_VIDEO && sws_outvid_ctx)
		{
			if (!sws_scale(sws_outvid_ctx, (const uint8_t * const*)frame->data, frame->linesize, 0, frame->height, sws_outvid_frame->data, sws_outvid_frame->linesize)) { av_log(NULL, AV_LOG_ERROR, "sws_scale outvid failed\n"); return -1; }
			sws_outvid_frame->pts = frame->pts;
			frame = sws_outvid_frame;
			frame->pict_type = AV_PICTURE_TYPE_NONE;
		}
		frame->pkt_dts = frame->pkt_pts = frame->pkt_pos = frame->best_effort_timestamp = AV_NOPTS_VALUE; frame->pkt_size = -1; //make sure this is not relevant (will be calculated for out below)
	}
	int (*enc_func)(AVCodecContext *, AVPacket *, const AVFrame *, int *) = (o_stream->codec->codec_type == AVMEDIA_TYPE_VIDEO ? avcodec_encode_video2 : avcodec_encode_audio2);
	int ret = enc_func(o_stream->codec, &enc_pkt, frame, got_frame);
	if (ret < 0) return ret;
	if (!*got_frame) return 0;

	//prepare packet for muxing
	enc_pkt.stream_index = o_stream->index;
	av_packet_rescale_ts(&enc_pkt, o_stream->codec->time_base, o_stream->time_base);

	//mux encoded frame
	return av_interleaved_write_frame(ofmt_ctx, &enc_pkt);
}

static int ThreadFrameWriter(const char* ofilename)
{
	sStreamStatus sto;
	if (!OpenOutput(sto, ofilename)) { OutputVideoCodecCtx.codec_type = OutputAudioCodecCtx.codec_type = AVMEDIA_TYPE_UNKNOWN; return 1; }
	OutputVideoCodecCtx = *sto.video->codec;
	OutputAudioCodecCtx = *sto.audio->codec;
	semFramesEncoded.Post();
	int stoOpenCount = 1;

	int64_t ptsAudio = 0, ptsVideo = 0;
	const int64_t cut_after_seconds = 60*60; // 1 HOUR
	int64_t ptsVideoCut = av_rescale(cut_after_seconds, sto.video->codec->time_base.den, sto.video->codec->time_base.num);
	while (semFramesToEncode.Wait())
	{
		mtxFramesToEncode.Lock();
		AVFrame* frame = framesToEncode.front().first;
		AVMediaType codec_type = framesToEncode.front().second;
		framesToEncode.erase(framesToEncode.begin());
		mtxFramesToEncode.Unlock();
		if (!frame) break;
		if (stoOpenCount)
		{
			if (codec_type == AVMEDIA_TYPE_VIDEO)
			{
				frame->pts = ptsVideo;
				ptsVideo += sto.video->codec->ticks_per_frame;
				EncodeWriteFrame(sto.fmt_ctx, frame, sto.video, NULL);
			}
			else
			{
				frame->pts = ptsAudio;
				ptsAudio += frame->nb_samples;
				EncodeWriteFrame(sto.fmt_ctx, frame, sto.audio, NULL);
			}
		}
		av_frame_free(&frame);
		semFramesEncoded.Post();

		if (ptsVideo >= ptsVideoCut && av_rescale_q(ptsAudio, sto.audio->codec->time_base, av_make_q(1, AV_TIME_BASE)) == av_rescale_q(ptsVideo, sto.video->codec->time_base, av_make_q(1, AV_TIME_BASE)))
		{
			show("=================================================================================================\n");
			if (sto.audio->codec->codec->capabilities & AV_CODEC_CAP_DELAY) for (int got_frame = 1; got_frame;) EncodeWriteFrame(sto.fmt_ctx, NULL, sto.audio, &got_frame);
			if (sto.video->codec->codec->capabilities & AV_CODEC_CAP_DELAY) for (int got_frame = 1; got_frame;) EncodeWriteFrame(sto.fmt_ctx, NULL, sto.video, &got_frame);
			CloseOutput(sto);
			ptsVideo = ptsAudio = 0;

			char* nextname = (char*)av_malloc(strlen(ofilename) + 64);
			const char* p = strrchr(ofilename, '.');
			sprintf(nextname, "%.*s_%d.%s", (p ? p-ofilename : strlen(ofilename)), ofilename, ++stoOpenCount, (p ? p+1 : "mkv"));
			show("=============== REOPEN OUTPUT FILE NUM %d =======================================================\n", stoOpenCount);
			if (!OpenOutput(sto, nextname))
			{
				//when reopen fails, this thread stays as it is but instead of encoding any more frames it just sucks all queued frames up and owner must end it
				stoOpenCount = 0;
				OutputVideoCodecCtx.codec_type = OutputAudioCodecCtx.codec_type = AVMEDIA_TYPE_UNKNOWN; //can be used outside thread to check for output error
			}
			av_free(nextname);
			show("=================================================================================================\n");
		}
	}

	if (OutputVideoCodecCtx.codec_type == AVMEDIA_TYPE_UNKNOWN) return 1; //error

	if (sto.audio->codec->codec->capabilities & AV_CODEC_CAP_DELAY) for (int got_frame = 1; got_frame;) EncodeWriteFrame(sto.fmt_ctx, NULL, sto.audio, &got_frame);
	if (sto.video->codec->codec->capabilities & AV_CODEC_CAP_DELAY) for (int got_frame = 1; got_frame;) EncodeWriteFrame(sto.fmt_ctx, NULL, sto.video, &got_frame);
	CloseOutput(sto);
	return 0;
}

static void push_encode_frame(AVFrame *frame, AVMediaType codec_type)
{
	static size_t queuedFrames = 0;
	while (semFramesEncoded.TryWait()) queuedFrames--;
	while (queuedFrames > 100) { semFramesEncoded.Wait(); queuedFrames--; }
	queuedFrames++;
	mtxFramesToEncode.Lock();
	framesToEncode.push_back(std::pair<AVFrame*, AVMediaType>(frame, codec_type));
	mtxFramesToEncode.Unlock();
	semFramesToEncode.Post();
}

static void FlushDecoders()
{
	mtxFramesToEncode.Lock();
	framesToEncode.push_back(std::pair<AVFrame*, AVMediaType>(NULL, AVMEDIA_TYPE_UNKNOWN));
	mtxFramesToEncode.Unlock();
	semFramesToEncode.Post();
}

static void ProcessMovingFrames(sFrameBuffering &fb)
{
	while (!fb.vid.backup_frames.empty() || !fb.aud.backup_frames.empty())
	{
		bool process_video = (fb.aud.backup_frames.empty() || (!fb.vid.backup_frames.empty() && fb.vid.backup_frames.front().ptsStream <= fb.aud.backup_frames.front().ptsStream));
		sFrameBuffering::sStream* frameBuffering = (process_video ? &fb.vid : &fb.aud);
		sFrameBuffering::sStream::sBackupFrame* bf = &frameBuffering->backup_frames.front();
		if (bf->ptsStream > fb.ptsMovingDetectionOutOfRange) break;

		bool freeInputFrame = true;
		if (process_video)
		{
			bool isInMovingRange = (bf->ptsStream >= fb.ptsMovingStarted && (fb.ptsMovingEnded < fb.ptsMovingStarted || bf->ptsStream < fb.ptsMovingEnded));
			if (isInMovingRange)
			{
				push_encode_frame(bf->frame, AVMEDIA_TYPE_VIDEO);
				freeInputFrame = false;
				fb.ptsDebugVideoFramesActuallyWritten++;
				//showlogonly("    V %6"PRId64" - PTS: %"PRId64"\n", fb.ptsDebugVideoFramesActuallyWritten, bf->ptsStream);
			}
		}
		else
		{
			int64_t ptsDuration = av_rescale_q(bf->frame->nb_samples, InputVideoCodecCtx.time_base, TimeBaseInputStream);
			int64_t audioTotal = av_rescale_q(fb.ptsAudioAmountVideoFrames * OutputVideoCodecCtx.ticks_per_frame, OutputVideoCodecCtx.time_base, OutputAudioCodecCtx.time_base);
			int64_t audioNeeded = audioTotal - fb.ptsAudioSamplesWritten;
			bool isInMovingRange = (bf->ptsStream + ptsDuration > fb.ptsMovingStarted && audioNeeded);
			if (isInMovingRange)
			{
				int64_t underflow = (bf->ptsStream < fb.ptsMovingStarted ? fb.ptsMovingStarted - bf->ptsStream : 0);
				int sampleOffset = (int)av_rescale_q(underflow, TimeBaseInputStream, InputVideoCodecCtx.time_base);

				for (int availableSamples = bf->frame->nb_samples - sampleOffset; availableSamples && audioNeeded;)
				{
					static AVFrame* audioBufFrame = NULL;
					if (!audioBufFrame)
					{
						audioBufFrame = av_frame_alloc();
						audioBufFrame->format = bf->frame->format;
						audioBufFrame->nb_samples = bf->frame->nb_samples;
						audioBufFrame->channel_layout = bf->frame->channel_layout;
						av_frame_get_buffer(audioBufFrame, 0);
						audioBufFrame->nb_samples = 0; //use as counter for how much we filled so far

						if (OutputAudioCodecCtx.frame_size == 0) OutputAudioCodecCtx.frame_size = bf->frame->nb_samples;
					}

					int consumeSamples = FFMIN((int)FFMIN((int64_t)availableSamples, audioNeeded), OutputAudioCodecCtx.frame_size - audioBufFrame->nb_samples);

					av_samples_copy(audioBufFrame->extended_data, bf->frame->extended_data, audioBufFrame->nb_samples, sampleOffset, consumeSamples, audioBufFrame->channels, (AVSampleFormat)audioBufFrame->format);

					sampleOffset += consumeSamples;
					audioBufFrame->nb_samples += consumeSamples;
					fb.ptsAudioSamplesWritten += consumeSamples;
					availableSamples -= consumeSamples;
					audioNeeded -= consumeSamples;

					//if full or at end of stream, write to output and try to read more samples
					if (audioBufFrame->nb_samples != OutputAudioCodecCtx.frame_size && fb.ptsMovingDetectionOutOfRange != INT64_MAX) break;

					push_encode_frame(audioBufFrame, AVMEDIA_TYPE_AUDIO); audioBufFrame = NULL;
				}
			}
		}
		if (freeInputFrame) av_frame_free(&bf->frame);
		frameBuffering->backup_frames.erase(frameBuffering->backup_frames.begin());
	}
}

static void FlushImageDetection(sFrameBuffering& fb)
{
	fb.ptsMovingDetectionOutOfRange = INT64_MAX; //eat up rest of audio video
	ProcessMovingFrames(fb);
}

static bool UpdateImageDetection(AVFrame* frame, int64_t ptsStream, sFrameBuffering& fb)
{
	enum { LASTNUM = 30, BLOCK_FRAMES_MIN = 20 }; //, BLOCK_FRAMES_MAX = 30*15 };
	struct sLastImageDetectionHistory { int pixdiff, pixwb; int64_t pts; };
	static sLastImageDetectionHistory last[LASTNUM];
	static int64_t currentBlockFrameCountStart = 0;
	static int64_t ptsMovingOldEnded = -1;

	static uint32_t tcStart;
	const int64_t frameCount = fb.vid.count;
	if (frameCount == 1) { tcStart = GetTicksU32(); }
	else if ((frameCount % 300) == 0) { show("Frame %d (fps: %f)\n", frameCount, ((double)frameCount * 1000.0 / (double)(GetTicksU32()-tcStart))); }

	//bilinear scale image to 64x64 grayscale for easier comparison
	if (!sws_scale(sws_imgdet_ctx, (const uint8_t * const*)frame->data, frame->linesize, 0, frame->height, sws_imgdet_data, sws_imgdet_linesize)) { av_log(NULL, AV_LOG_ERROR, "sws_scale imgdet failed\n"); return false; }

	//find number of pixels different from last image (also find all fully-white/black pixels to avoid flashing issues present in source)
	int pixdiff = 0, pixwb = 0;
	for (int y = 5; y < 5+58; y++) { for (int i = y*sws_imgdet_linesize[0]+1, iEnd = i+40; i != iEnd; i++)
	{
		if (sws_imgdet_data[0][i] > 253 || sws_imgdet_data[0][i] < 2) pixwb++;
		if (abs(sws_imgdet_data[0][i] - sws_imgdet_prev_data[0][i]) > 3) pixdiff++;
	} }
	memcpy(sws_imgdet_prev_data[0], sws_imgdet_data[0], 64*sws_imgdet_linesize[0]);
	
	//shift the last[] array one forward and write current info into [0]
	memmove(&last[1], &last[0], sizeof(last)-sizeof(last[0]));
	last[0].pixdiff = pixdiff;
	last[0].pixwb = pixwb;
	last[0].pts = ptsStream;
	if (frameCount < LASTNUM) return true;

	//decide based on number of changed pixels in the last few frames where movement starts/ends
	if (currentBlockFrameCountStart == 0)
	{
		int diff_frames = 0, start_diff = 0;
		for (int i = 0; i < LASTNUM; i++)
		{
			if (last[i].pixwb > 2200) break;
			if (last[i].pixdiff > 10) diff_frames++;
			if (last[i].pixdiff > 100) start_diff = i; 
		}
		if (diff_frames > 6 && last[start_diff].pts <= fb.ptsMovingEnded)
		{
			fb.ptsMovingEnded = ptsMovingOldEnded;
			currentBlockFrameCountStart = frameCount-start_diff;
			show("---------- CONTINUE @ %d (PTS: %"PRId64") ----------\n", frameCount-start_diff, last[start_diff].pts);
			//for (int i = LASTNUM-1; i >= 0; i--) showlogonly("           F: %6d - PTS: %"PRId64" - PIXDIFF: %4d - PIXWB: %4d\n", frameCount-i, last[i].pts, last[i].pixdiff, last[i].pixwb);
		}
		else if (diff_frames > 6 && last[start_diff].pts > fb.ptsMovingEnded)
		{
			fb.ptsMovingStarted = last[start_diff].pts;
			currentBlockFrameCountStart = frameCount-start_diff;

			int64_t audioTotal = av_rescale_q(fb.ptsAudioAmountVideoFrames * OutputVideoCodecCtx.ticks_per_frame, OutputVideoCodecCtx.time_base, OutputAudioCodecCtx.time_base);
			show("---------- START @ %8d (PTS: %"PRId64") ---------- (VidFrames: %"PRId64" - AudioFrames: %"PRId64" - AudioTotal: %"PRId64" - Needed = %"PRId64") \n", frameCount-start_diff, fb.ptsMovingStarted
				, fb.ptsDebugVideoFramesActuallyWritten, fb.ptsAudioAmountVideoFrames, audioTotal, audioTotal - fb.ptsAudioSamplesWritten);
			//for (int i = LASTNUM-1; i >= 0; i--) showlogonly("           F: %6d - PTS: %"PRId64" - PIXDIFF: %4d - PIXWB: %4d\n", frameCount-i, last[i].pts, last[i].pixdiff, last[i].pixwb);
			if (fb.ptsDebugVideoFramesActuallyWritten != fb.ptsAudioAmountVideoFrames) { show("\n\nERROR! VideoFramesActuallyWritten AND ptsAudioAmountVideoFrames MISMATCH!\n\n\n"); return false; }
		}
	}
	else
	{
		int diff_frames = 0, end_diff = LASTNUM-1;
		for (int i = 1; i < LASTNUM; i++)
		{
			if (last[i].pixdiff > 10) diff_frames++;
			if (last[i].pixdiff > 100 && end_diff == LASTNUM-1) end_diff = i;
			if (last[i].pixwb > 2200) end_diff = LASTNUM-1;
		}
		const bool alreadyEnded = (fb.ptsMovingStarted < fb.ptsMovingEnded);
		if (diff_frames < 7 && last[end_diff].pts > fb.ptsMovingStarted && (frameCount-(end_diff-1) - currentBlockFrameCountStart) >= BLOCK_FRAMES_MIN)
		{
			if (!alreadyEnded) { ptsMovingOldEnded = fb.ptsMovingEnded; fb.ptsMovingEnded = last[end_diff-1].pts; }
			//for (int i = LASTNUM-1; i >= 0; i--) showlogonly("           F: %6d - PTS: %"PRId64" - PIXDIFF: %4d - PIXWB: %4d\n", frameCount-i, last[i].pts, last[i].pixdiff, last[i].pixwb);
			show("---------- END   @ %8d (PTS: %"PRId64") ---------- (This Block Count: %"PRId64")\n\n", frameCount-(end_diff-1), fb.ptsMovingEnded, frameCount-(end_diff-1) - currentBlockFrameCountStart);
			currentBlockFrameCountStart = 0;
		}
		#if 0 || USE_BLOCK_FRAMES_MAX_LIMIT
		else if (frameCount-(LASTNUM-1) - currentBlockFrameCountStart >= BLOCK_FRAMES_MAX && !alreadyEnded)
		{
			ptsMovingOldEnded = fb.ptsMovingEnded; fb.ptsMovingEnded = last[LASTNUM-1].pts;
			//for (int i = LASTNUM-1; i >= 0; i--) showlogonly("           F: %6d - PTS: %"PRId64" - PIXDIFF: %4d - PIXWB: %4d\n", frameCount-i, last[i].pts, last[i].pixdiff, last[i].pixwb);
			show("---------- MAX   @ %8d (PTS: %"PRId64") ---------- (This Block Count: %"PRId64")\n\n", frameCount-(LASTNUM-1), fb.ptsMovingEnded, frameCount-(LASTNUM-1) - currentBlockFrameCountStart);
		}
		#endif
	}

	if (last[LASTNUM-1].pts >= fb.ptsMovingStarted && (fb.ptsMovingStarted > fb.ptsMovingEnded || last[LASTNUM-1].pts < fb.ptsMovingEnded))
	{
		fb.ptsAudioAmountVideoFrames++;
		//showlogonly("           MARKED FOR OUTPUT %d (PTS: %"PRId64") ---------- (AudioAmountFrames: %"PRId64")\n", frameCount-LASTNUM+1, last[LASTNUM-1].pts, ptsAudioAmountVideoFrames);
		//showlogonly("F: %6d - O: %6d - PIXDIFF: %4d - PIXWB: %4d\n", frameCount, ptsAudioAmountVideoFrames, last[LASTNUM-1].pixdiff, last[LASTNUM-1].pixwb);
		//showlogonly("    A %6"PRId64" - PTS: %"PRId64"\n", fb.ptsAudioAmountVideoFrames, last[LASTNUM-1].pts);
	}
	//else if (pixdiff || pixwb) { printf("F: %6d - O: ----- - PIXDIFF: %4d - PIXWB: %4d\n", frameCount, pixdiff, pixwb); }

	fb.ptsMovingDetectionOutOfRange = last[LASTNUM-1].pts;
	return true;
}

static bool DecodeFrame(const AVPacket* pkt, AVFrame* frame, AVStream* i_stream, sFrameBuffering& fb, int* got_frame = NULL)
{
	static int64_t ptsShift = 0;

	int local_got_frame; if (!got_frame) got_frame = &local_got_frame;

	bool isVideo = (i_stream->codec->codec_type == AVMEDIA_TYPE_VIDEO);
	int(*dec_func)(AVCodecContext *, AVFrame *, int *, const AVPacket *) = (isVideo ? avcodec_decode_video2 : avcodec_decode_audio4);
	if (dec_func(i_stream->codec, frame, got_frame, pkt) < 0) { av_frame_unref(frame); show("Error: Decoding failed\n"); return false; }
	if (!*got_frame) { av_frame_unref(frame); return true; }
	frame->pts = av_frame_get_best_effort_timestamp(frame);
	
	sFrameBuffering::sStream* frameBuffering = (isVideo ? &fb.vid : &fb.aud);
	frameBuffering->count++;

	sFrameBuffering::sStream::sBackupFrame bf;
	bf.ptsStream = ptsShift + av_rescale_q(frame->pts, i_stream->codec->time_base, i_stream->time_base);
	//show(" PTS ON %s FRAME %7"PRId64" = %"PRId64"\n", (isVideo ? "VIDEO" : "AUDIO"), frameBuffering->count, bf.ptsStream);

	size_t numbf = frameBuffering->backup_frames.size();
	if (numbf)
	{
		sFrameBuffering::sStream::sBackupFrame& prefbf = frameBuffering->backup_frames.back();
		int64_t duration = (frame->pkt_duration == 0 || frame->pkt_duration == AV_NOPTS_VALUE ? 0 : av_rescale_q(frame->pkt_duration, i_stream->codec->time_base, i_stream->time_base));
		if (!duration) duration  = av_rescale_q((isVideo ? i_stream->codec->ticks_per_frame : frame->nb_samples), i_stream->codec->time_base, i_stream->time_base);
		if (!duration && numbf > 1) duration = prefbf.ptsStream - frameBuffering->backup_frames[numbf-2].ptsStream;
		if (duration > 0 && (bf.ptsStream < prefbf.ptsStream || bf.ptsStream > prefbf.ptsStream + duration*100))
		{
			ptsShift += (prefbf.ptsStream - bf.ptsStream) + duration;
			show("===================\nSHIFTING BAD PTS ON %s FRAME %7"PRId64"!\n", (isVideo ? "VIDEO" : "AUDIO"), frameBuffering->count);
			show("    PREV FRAME: %"PRId64" (SRC ST: %"PRId64" - CH: %"PRId64")\n", prefbf.ptsStream, av_rescale_q(prefbf.frame->pts, i_stream->codec->time_base, i_stream->time_base), prefbf.frame->pts);
			show("    THIS FRAME: %"PRId64" (SRC ST: %"PRId64" - CH: %"PRId64")\n", bf.ptsStream, av_rescale_q(frame->pts, i_stream->codec->time_base, i_stream->time_base), frame->pts);
			show("    DIFF: %"PRId64"\n    FRAME DURATION: %"PRId64"\n    ADD SHIFTING: %"PRId64" (TOTAL: %"PRId64")\n===================\n", (prefbf.ptsStream - bf.ptsStream), duration, (prefbf.ptsStream - bf.ptsStream) + duration, ptsShift);
			bf.ptsStream += (prefbf.ptsStream - bf.ptsStream) + duration;
		}
		else if (bf.ptsStream < prefbf.ptsStream)
		{
			show("===================\nFIXING BAD PTS ON %s FRAME %7"PRId64"!\n", (isVideo ? "VIDEO" : "AUDIO"), frameBuffering->count);
			show("    PREV FRAME: %"PRId64" (SRC ST: %"PRId64" - CH: %"PRId64")\n", prefbf.ptsStream, av_rescale_q(prefbf.frame->pts, i_stream->codec->time_base, i_stream->time_base), prefbf.frame->pts);
			show("    THIS FRAME: %"PRId64" (SRC ST: %"PRId64" - CH: %"PRId64")\n", bf.ptsStream, av_rescale_q(frame->pts, i_stream->codec->time_base, i_stream->time_base), frame->pts);
			show("    DIFF: %"PRId64"\n    FRAME DURATION: %"PRId64"\n===================\n", (prefbf.ptsStream - bf.ptsStream), duration);
			bf.ptsStream = prefbf.ptsStream;
		}
	}

	bf.frame = av_frame_clone(frame);
	frameBuffering->backup_frames.push_back(bf);

	bool ret = true;
	if (isVideo && (ret = UpdateImageDetection(frame, bf.ptsStream, fb)))
		ProcessMovingFrames(fb);

	av_frame_unref(frame);
	return ret;
}

int main(int argc, char *argv[])
{
	if (argc < 2) { fprintf(stderr, "Run:\n%s <OUTPUT_FILE>\n\n", (argc ? argv[0] : "detect_video")); return 9; }
	const char* ofilename = argv[1];

	if (strlen(ofilename) < 500) { char buf[512]; sprintf(buf, "%s.log", ofilename); fLogFile = fopen(buf, "w"); }
	av_log_set_callback(logcallback);
	show("Encoding from STDIN to \"%s\"...\n\n", ofilename);

	av_register_all(); //register codecs and formats and other lavf/lavc components

	sStreamStatus sti;
	if (!OpenInput(sti)) return 1;
	TimeBaseInputStream = sti.audio->time_base;
	InputVideoCodecCtx = *sti.video->codec;
	InputAudioCodecCtx = *sti.audio->codec;

	if (!SetupSWS()) return 1;

	#define THREAD_OUTPUT_HAS_BROKEN (OutputVideoCodecCtx.codec_type != AVMEDIA_TYPE_VIDEO)
	Thread thrFramesToEncode((Thread::FuncPtr)ThreadFrameWriter, ofilename);
	semFramesEncoded.Wait(); //wait until output opened by thread
	if (THREAD_OUTPUT_HAS_BROKEN) return 1; //could not open output

	sFrameBuffering fb;

	AVFrame* frame = av_frame_alloc();

	while (packetReadingInputIsValid)
	{
		//initialize packet, set data to NULL, let the demuxer fill it
		AVPacket pkt;
		av_init_packet(&pkt);
		pkt.data = NULL, pkt.size = 0;
	
		for (; av_read_frame(sti.fmt_ctx, &pkt) >= 0; av_free_packet(&pkt))
		{
			if (pkt.stream_index != sti.video->index && pkt.stream_index != sti.audio->index) continue;
			av_packet_rescale_ts(&pkt, sti.fmt_ctx->streams[pkt.stream_index]->time_base, sti.fmt_ctx->streams[pkt.stream_index]->codec->time_base);
			if (!DecodeFrame(&pkt, frame, sti.fmt_ctx->streams[pkt.stream_index], fb)) { av_log(NULL, AV_LOG_ERROR, "DecodeFrame failed, finishing early\n"); goto FinishedDecode; }
			if (THREAD_OUTPUT_HAS_BROKEN) { av_log(NULL, AV_LOG_ERROR, "Encode thread failed, finishing early\n"); goto FinishedDecode; }
		}

		//flush cached frames
		for (unsigned int stream_index = 0; stream_index < sti.fmt_ctx->nb_streams; stream_index++)
			if (stream_index == sti.video->index || stream_index == sti.audio->index)
				for (int got_frame = 0; DecodeFrame(&pkt, frame, sti.fmt_ctx->streams[pkt.stream_index], fb, &got_frame) && got_frame;) { }

		FinishedDecode:
		av_free_packet(&pkt); //error cases, otherwise does nothing
		CloseInput(sti);
		if (THREAD_OUTPUT_HAS_BROKEN) break;

		//try to recover if there is still stuff being written to us
		if (packetReadingInputIsValid)
		{
			show("Reading input stream or decoding has stopped but input is still valid. Trying to reopen and continue...\n");
			if (!OpenInput(sti)) break;
		}
	}

	av_frame_free(&frame);
	FlushImageDetection(fb);
	FlushDecoders();
	thrFramesToEncode.Wait();
	FreeSWS();

	if (fLogFile) fclose(fLogFile);

	return 0;
}
