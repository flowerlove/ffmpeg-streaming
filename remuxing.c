/*
 Remux streams from one container format to another.
 mingw: download dll and dev packages from http://ffmpeg.zeranoe.com/builds,
 merge to /e/tools/Player/ffmpeg/, and then invoke:
 
 gcc remuxing.c -g -O0  \
 -I /e/tools/Player/ffmpeg/include \
 -L /e/tools/Player/ffmpeg/lib \
 -lavdevice -lavfilter -lavformat -lavcodec -lpostproc -lswresample -lswscale -lavutil 
 
 linux: -lm -lz -lpthread -lbz2 -lrt -L /usr/lib64 
 */

#include <libavutil/timestamp.h>
#include <libavformat/avformat.h>
#include <libavutil/time.h>

#include "ff_utils.c"

#define M 4
static int do_sff = 1;
static int do_rate_emu = 1;
static int64_t start_time[M], curr_time[M];
static int64_t first_dts[M] = {0};
static int64_t curr_dts[M] = {AV_NOPTS_VALUE, AV_NOPTS_VALUE, AV_NOPTS_VALUE, AV_NOPTS_VALUE};

int main(int argc, char **argv)
{
    AVOutputFormat *ofmt = NULL;
    AVFormatContext *ifmt_ctx = NULL, *ofmt_ctx = NULL;
	AVDictionary *out_opts = NULL;
    AVPacket pkt;
    const char *fmt, *in_filename, *out_filename;
    int ret, i, cmp;
	int cnt = 0;

    if (argc < 4) {
        printf("usage: remuxing input output fmt [do_sff do_rate_emu]\n");
        return 1;
    }
	if(argc >= 5){
		do_sff = atoi(argv[4]);
	}
	if(argc >= 6){
		do_rate_emu = atoi(argv[5]);
	}

    in_filename  = argv[1];
    out_filename = argv[2];
	fmt = argv[3];

    av_register_all();
    avformat_network_init();

    if ((ret = avformat_open_input(&ifmt_ctx, in_filename, 0, 0)) < 0) {
        fprintf(stderr, "Could not open input file '%s'", in_filename);
        goto end;
    }

    if ((ret = avformat_find_stream_info(ifmt_ctx, 0)) < 0) {
        fprintf(stderr, "Failed to retrieve input stream information");
        goto end;
    }

    av_dump_format(ifmt_ctx, 0, in_filename, 0);

    avformat_alloc_output_context2(&ofmt_ctx, NULL, fmt, out_filename);
    if (!ofmt_ctx) {
        fprintf(stderr, "Could not create output context\n");
        ret = AVERROR_UNKNOWN;
        goto end;
    }

    ofmt = ofmt_ctx->oformat;

    for (i = 0; i < ifmt_ctx->nb_streams; i++) {
        AVStream *in_stream = ifmt_ctx->streams[i];
        AVStream *out_stream = avformat_new_stream(ofmt_ctx, in_stream->codec->codec);
        if (!out_stream) {
            fprintf(stderr, "Failed allocating output stream\n");
            ret = AVERROR_UNKNOWN;
            goto end;
        }
		
		avpriv_set_pts_info(out_stream, in_stream->pts_wrap_bits, in_stream->time_base.num, in_stream->time_base.den);
        ret = avcodec_copy_context(out_stream->codec, in_stream->codec);
        if (ret < 0) {
            fprintf(stderr, "Failed to copy context from input to output stream codec context\n");
            goto end;
        }
        out_stream->codec->codec_tag = 0;
        if (ofmt_ctx->oformat->flags & AVFMT_GLOBALHEADER)
            out_stream->codec->flags |= CODEC_FLAG_GLOBAL_HEADER;
    }

	if(out_filename && strstr(out_filename, ".m3u8")){
		av_opt_set_int(ofmt_ctx, "hls_wrap",  6, AV_OPT_SEARCH_CHILDREN);
		av_opt_set_int(ofmt_ctx, "hls_list_size",  6, AV_OPT_SEARCH_CHILDREN); 
		av_opt_set(ofmt_ctx, "hls_time", "1.0", AV_OPT_SEARCH_CHILDREN);
	}

    av_dump_format(ofmt_ctx, 0, out_filename, 1);

    if (!(ofmt->flags & AVFMT_NOFILE)) {
		av_dict_set(&out_opts, "chunked_post", "0", 0);	
        ret = avio_open2(&ofmt_ctx->pb, out_filename, AVIO_FLAG_WRITE|AVIO_FLAG_NONBLOCK, NULL, &out_opts);

        if (ret < 0) {
            fprintf(stderr, "Could not open output file '%s'", out_filename);
            goto end;
        }
    }

    ret = ofmt_ctx->pb && do_sff ? sff_write_header(ofmt_ctx) : avformat_write_header(ofmt_ctx, NULL);
    if (ret < 0) {
        fprintf(stderr, "Error occurred when opening output file\n");
        goto end;
    }
	if(ofmt_ctx->pb){
		avio_flush(ofmt_ctx->pb);
	}

    while (1) {
        AVStream *in_stream, *out_stream;
		ret = av_read_frame(ifmt_ctx, &pkt);
        if (ret < 0)
            break;

        in_stream  = ifmt_ctx->streams[pkt.stream_index];
        out_stream = ofmt_ctx->streams[pkt.stream_index];
		
		i = pkt.stream_index;
		if(curr_dts[i] == AV_NOPTS_VALUE && pkt.dts != AV_NOPTS_VALUE){
			first_dts[i] = pkt.dts;
			start_time[i] = av_gettime_relative();
		}
		if(pkt.dts != AV_NOPTS_VALUE){	
			curr_dts[i] = pkt.dts; //us
		}

        /* copy packet */
        pkt.pts = av_rescale_q_rnd(pkt.pts, in_stream->time_base, out_stream->time_base, AV_ROUND_NEAR_INF|AV_ROUND_PASS_MINMAX);
        pkt.dts = av_rescale_q_rnd(pkt.dts, in_stream->time_base, out_stream->time_base, AV_ROUND_NEAR_INF|AV_ROUND_PASS_MINMAX);
        pkt.duration = av_rescale_q(pkt.duration, in_stream->time_base, out_stream->time_base);
        pkt.pos = -1;
		
		ret = ofmt_ctx->pb && do_sff ? sff_write_packet(ofmt_ctx, &pkt) : av_interleaved_write_frame(ofmt_ctx, &pkt); 
		if(ofmt_ctx->pb){
			avio_flush(ofmt_ctx->pb);
		}
        if (ret < 0) {
            fprintf(stderr, "Error muxing packet\n");
            break;
        }
        av_free_packet(&pkt);
		++cnt;
		//printf("cnt %d\t", cnt);
		
		do{
			curr_time[i] = av_gettime_relative();
			cmp = av_compare_ts(curr_dts[i] - first_dts[i], in_stream->time_base, 
					curr_time[i] - start_time[i], AV_TIME_BASE_Q); 
			if(!do_rate_emu || cmp <= 0)break;
			
			av_usleep(10000);
		}while(cmp > 0);
		
    }
	
	ofmt_ctx->pb && do_sff ? sff_write_packet(ofmt_ctx, NULL) : av_write_trailer(ofmt_ctx); 
end:

    avformat_close_input(&ifmt_ctx);

    /* close output */
    if (ofmt_ctx && !(ofmt->flags & AVFMT_NOFILE) && ofmt_ctx->pb){
        avio_close(ofmt_ctx->pb);
		av_dict_free(&out_opts);
	}
    avformat_free_context(ofmt_ctx);

    if (ret < 0 && ret != AVERROR_EOF) {
        fprintf(stderr, "Error occurred: %s\n", av_err2str(ret));
        return 1;
    }
	
	printf("end of remux\n");
    return 0;
}
