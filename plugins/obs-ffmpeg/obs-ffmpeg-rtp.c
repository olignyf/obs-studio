/*
 * Native RTP streaming output:
 *   - Video: HEVC / H.265 per RFC 7798 (FFmpeg rtp muxer)
 *   - Audio: raw PCM as RTP L16 (pcm_s16be), second RTP session on port+2
 *
 * FFmpeg's "rtp" muxer only allows one elementary stream per URL, so A/V use
 * two RTP sessions. Custom service URL: rtp://host:port
 */

#include <obs-module.h>
#include <util/threading.h>
#include <util/darray.h>
#include <util/dstr.h>
#include <media-io/audio-io.h>

#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>

#include "obs-ffmpeg-formats.h"
#include "obs-ffmpeg-compat.h"

static void *rtp_write_thread(void *data);

#define do_log(level, format, ...) blog(level, "[obs-ffmpeg rtp]: " format, ##__VA_ARGS__)
#define warn(format, ...) do_log(LOG_WARNING, format, ##__VA_ARGS__)
#define info(format, ...) do_log(LOG_INFO, format, ##__VA_ARGS__)
#define error(format, ...) do_log(LOG_ERROR, format, ##__VA_ARGS__)

struct rtp_session {
	AVFormatContext *fmt;
	AVStream *stream;
	char *url;
};

struct rtp_output {
	obs_output_t *output;

	struct rtp_session video;
	struct rtp_session audio;

	volatile bool active;
	volatile bool stopping;
	uint64_t stop_ts;
	uint64_t total_bytes;

	bool write_thread_active;
	pthread_t write_thread;
	pthread_mutex_t write_mutex;
	os_sem_t *write_sem;
	os_event_t *stop_event;

	DARRAY(AVPacket *) packets;

	AVRational video_tb;
	AVRational audio_tb;
	int audio_channels;
	int audio_rate;
	bool audio_is_le; /* OBS pcm_s16le → swap to BE for L16 */
};

static const char *rtp_getname(void *unused)
{
	UNUSED_PARAMETER(unused);
	return "FFmpeg RTP (RFC 7798 HEVC + L16 PCM)";
}

static inline bool rtp_stopping(struct rtp_output *out)
{
	return os_atomic_load_bool(&out->stopping);
}

static void free_session(struct rtp_session *s)
{
	if (s->fmt) {
		if (s->fmt->pb)
			avio_closep(&s->fmt->pb);
		avformat_free_context(s->fmt);
		s->fmt = NULL;
		s->stream = NULL;
	}
	bfree(s->url);
	s->url = NULL;
}

static void rtp_destroy(void *data)
{
	struct rtp_output *out = data;
	if (!out)
		return;

	if (os_atomic_load_bool(&out->active)) {
		os_atomic_set_bool(&out->stopping, true);
		os_event_signal(out->stop_event);
		os_sem_post(out->write_sem);
		if (out->write_thread_active) {
			pthread_join(out->write_thread, NULL);
			out->write_thread_active = false;
		}
		os_atomic_set_bool(&out->active, false);
	}

	pthread_mutex_lock(&out->write_mutex);
	for (size_t i = 0; i < out->packets.num; i++)
		av_packet_free(out->packets.array + i);
	da_free(out->packets);
	pthread_mutex_unlock(&out->write_mutex);

	free_session(&out->video);
	free_session(&out->audio);

	os_event_destroy(out->stop_event);
	os_sem_destroy(out->write_sem);
	pthread_mutex_destroy(&out->write_mutex);
	bfree(out);
}

static void *rtp_create(obs_data_t *settings, obs_output_t *output)
{
	struct rtp_output *out = bzalloc(sizeof(*out));
	UNUSED_PARAMETER(settings);
	out->output = output;
	pthread_mutex_init(&out->write_mutex, NULL);
	os_event_init(&out->stop_event, OS_EVENT_TYPE_AUTO);
	os_sem_init(&out->write_sem, 0);
	da_init(out->packets);
	return out;
}

/* rtp://host:5004 → audio on 5006 (ffmpeg convention: port+2) */
static bool build_audio_url(const char *video_url, char **audio_url_out)
{
	struct dstr dst = {0};
	dstr_copy(&dst, video_url);

	const char *scheme = strstr(video_url, "rtp://");
	if (!scheme) {
		dstr_free(&dst);
		return false;
	}

	const char *host = video_url + 6;
	const char *colon = strrchr(host, ':');
	if (!colon) {
		dstr_free(&dst);
		return false;
	}

	int port = atoi(colon + 1);
	if (port <= 0 || port > 65533) {
		dstr_free(&dst);
		return false;
	}

	dstr_resize(&dst, (size_t)(colon - video_url));
	dstr_catf(&dst, ":%d", port + 2);
	*audio_url_out = bstrdup(dst.array);
	dstr_free(&dst);
	return true;
}

static int open_rtp_session(struct rtp_session *s, const char *url, enum AVMediaType type, enum AVCodecID codec_id,
			    int width, int height, int sample_rate, int channels, AVRational tb)
{
	int ret;
	s->url = bstrdup(url);
	ret = avformat_alloc_output_context2(&s->fmt, NULL, "rtp", url);
	if (ret < 0 || !s->fmt) {
		warn("avformat_alloc_output_context2 failed for %s: %s", url, av_err2str(ret));
		return ret < 0 ? ret : AVERROR_UNKNOWN;
	}

	s->stream = avformat_new_stream(s->fmt, NULL);
	if (!s->stream)
		return AVERROR(ENOMEM);

	s->stream->id = 0;
	s->stream->time_base = tb;
	AVCodecParameters *par = s->stream->codecpar;
	par->codec_type = type;
	par->codec_id = codec_id;

	if (type == AVMEDIA_TYPE_VIDEO) {
		par->width = width;
		par->height = height;
	} else {
		par->sample_rate = sample_rate;
		par->format = AV_SAMPLE_FMT_S16;
		av_channel_layout_default(&par->ch_layout, channels);
		par->bits_per_coded_sample = 16;
		par->bit_rate = (int64_t)sample_rate * channels * 16;
	}

	ret = avio_open(&s->fmt->pb, url, AVIO_FLAG_WRITE);
	if (ret < 0) {
		warn("avio_open(%s) failed: %s", url, av_err2str(ret));
		return ret;
	}

	AVDictionary *opts = NULL;
	ret = avformat_write_header(s->fmt, &opts);
	av_dict_free(&opts);
	if (ret < 0) {
		warn("write_header(%s) failed: %s", url, av_err2str(ret));
		return ret;
	}

	return 0;
}

static void log_sdp(struct rtp_output *out, const char *base_url)
{
	/* Build a minimal SDP matching ffmpeg -sdp_file for dual RTP. */
	struct dstr sdp = {0};
	const char *host = "0.0.0.0";
	int vport = 5004;

	const char *p = strstr(base_url, "rtp://");
	if (p) {
		p += 6;
		char hostbuf[256];
		const char *colon = strrchr(p, ':');
		if (colon) {
			size_t n = (size_t)(colon - p);
			if (n >= sizeof(hostbuf))
				n = sizeof(hostbuf) - 1;
			memcpy(hostbuf, p, n);
			hostbuf[n] = 0;
			/* strip [ipv6] brackets if present */
			host = hostbuf;
			vport = atoi(colon + 1);
		}
	}

	dstr_printf(&sdp,
		    "v=0\r\n"
		    "o=- 0 0 IN IP4 %s\r\n"
		    "s=OBS RTP\r\n"
		    "t=0 0\r\n"
		    "m=video %d RTP/AVP 96\r\n"
		    "c=IN IP4 %s\r\n"
		    "a=rtpmap:96 H265/90000\r\n"
		    "m=audio %d RTP/AVP 97\r\n"
		    "c=IN IP4 %s\r\n"
		    "a=rtpmap:97 L16/%d/%d\r\n",
		    host, vport, host, vport + 2, host, out->audio_rate, out->audio_channels);

	info("SDP (RFC 7798 HEVC + L16 PCM):\n%s", sdp.array);
	dstr_free(&sdp);
}

static bool rtp_start(void *data)
{
	struct rtp_output *out = data;
	obs_service_t *service = obs_output_get_service(out->output);
	if (!service) {
		warn("No service");
		return false;
	}

	const char *url = obs_service_get_connect_info(service, OBS_SERVICE_CONNECT_INFO_SERVER_URL);
	if (!url || strncmp(url, "rtp://", 6) != 0) {
		warn("URL must be rtp://host:port (got '%s')", url ? url : "(null)");
		obs_output_set_last_error(out->output, "RTP URL must start with rtp://");
		return false;
	}

	obs_encoder_t *venc = obs_output_get_video_encoder(out->output);
	obs_encoder_t *aenc = obs_output_get_audio_encoder(out->output, 0);
	if (!venc || !aenc) {
		warn("Missing encoders");
		return false;
	}

	const char *vcodec = obs_encoder_get_codec(venc);
	const char *acodec = obs_encoder_get_codec(aenc);
	if (!vcodec || (strcmp(vcodec, "hevc") != 0 && strcmp(vcodec, "h265") != 0)) {
		warn("RFC 7798 RTP video requires HEVC (got '%s')", vcodec ? vcodec : "?");
		obs_output_set_last_error(out->output, "RTP (RFC 7798) requires HEVC video encoder");
		return false;
	}
	if (!acodec || (strcmp(acodec, "pcm_s16le") != 0 && strcmp(acodec, "pcm_bluray") != 0 &&
			strcmp(acodec, "pcm_s16be") != 0)) {
		warn("RTP audio requires raw PCM (got '%s')", acodec ? acodec : "?");
		obs_output_set_last_error(out->output, "RTP requires RAW PCM audio encoder");
		return false;
	}

	video_t *video = obs_output_video(out->output);
	const struct video_output_info *voi = video_output_get_info(video);
	struct obs_audio_info aoi;
	if (!obs_get_audio_info(&aoi))
		return false;

	out->audio_rate = (int)aoi.samples_per_sec;
	out->audio_channels = (int)get_audio_channels(aoi.speakers);
	out->audio_is_le = (strcmp(acodec, "pcm_s16be") != 0);
	out->video_tb = (AVRational){voi->fps_den, voi->fps_num};
	out->audio_tb = (AVRational){1, out->audio_rate};

	char *audio_url = NULL;
	if (!build_audio_url(url, &audio_url)) {
		warn("Could not derive audio RTP URL from '%s'", url);
		return false;
	}

	free_session(&out->video);
	free_session(&out->audio);

	int ret = open_rtp_session(&out->video, url, AVMEDIA_TYPE_VIDEO, AV_CODEC_ID_HEVC, (int)obs_output_get_width(out->output),
				   (int)obs_output_get_height(out->output), 0, 0, out->video_tb);
	if (ret < 0) {
		bfree(audio_url);
		free_session(&out->video);
		return false;
	}

	ret = open_rtp_session(&out->audio, audio_url, AVMEDIA_TYPE_AUDIO, AV_CODEC_ID_PCM_S16BE, 0, 0, out->audio_rate,
			       out->audio_channels, out->audio_tb);
	bfree(audio_url);
	if (ret < 0) {
		free_session(&out->video);
		free_session(&out->audio);
		return false;
	}

	/* Ensure HEVC extradata (VPS/SPS/PPS) for SDP/out-of-band if available. */
	uint8_t *extra = NULL;
	size_t extra_size = 0;
	if (obs_encoder_get_extra_data(venc, &extra, &extra_size) && extra && extra_size) {
		out->video.stream->codecpar->extradata = av_memdup(extra, extra_size + AV_INPUT_BUFFER_PADDING_SIZE);
		out->video.stream->codecpar->extradata_size = (int)extra_size;
	}

	log_sdp(out, url);
	info("Streaming video %s + audio %s", out->video.url, out->audio.url);

	if (!obs_output_can_begin_data_capture(out->output, 0))
		goto fail;
	if (!obs_output_initialize_encoders(out->output, 0))
		goto fail;

	os_event_reset(out->stop_event);
	os_atomic_set_bool(&out->stopping, false);
	out->total_bytes = 0;

	ret = pthread_create(&out->write_thread, NULL, rtp_write_thread, out);
	if (ret != 0) {
		warn("Failed to create write thread");
		goto fail;
	}
	out->write_thread_active = true;
	os_atomic_set_bool(&out->active, true);
	obs_output_begin_data_capture(out->output, 0);
	return true;

fail:
	free_session(&out->video);
	free_session(&out->audio);
	return false;
}

static void *rtp_write_thread(void *data)
{
	struct rtp_output *out = data;

	while (os_sem_wait(out->write_sem) == 0) {
		if (os_event_try(out->stop_event) == 0)
			break;

		AVPacket *pkt = NULL;
		pthread_mutex_lock(&out->write_mutex);
		if (out->packets.num) {
			pkt = out->packets.array[0];
			da_erase(out->packets, 0);
		}
		pthread_mutex_unlock(&out->write_mutex);
		if (!pkt)
			continue;

		struct rtp_session *sess = (pkt->stream_index == 0) ? &out->video : &out->audio;
		pkt->stream_index = 0;
		int ret = av_interleaved_write_frame(sess->fmt, pkt);
		if (ret < 0)
			warn("write_frame failed: %s", av_err2str(ret));
		else
			out->total_bytes += pkt->size;
		av_packet_free(&pkt);
	}
	return NULL;
}

static void rtp_stop(void *data, uint64_t ts)
{
	struct rtp_output *out = data;
	if (!os_atomic_load_bool(&out->active))
		return;

	if (ts > 0) {
		out->stop_ts = ts;
		os_atomic_set_bool(&out->stopping, true);
	} else {
		os_atomic_set_bool(&out->stopping, true);
		os_event_signal(out->stop_event);
		os_sem_post(out->write_sem);
		if (out->write_thread_active) {
			pthread_join(out->write_thread, NULL);
			out->write_thread_active = false;
		}
		if (out->video.fmt)
			av_write_trailer(out->video.fmt);
		if (out->audio.fmt)
			av_write_trailer(out->audio.fmt);
		free_session(&out->video);
		free_session(&out->audio);
		os_atomic_set_bool(&out->active, false);
		obs_output_signal_stop(out->output, OBS_OUTPUT_SUCCESS);
	}
}

static inline int64_t rescale_rtp(int64_t val, AVRational from, AVRational to)
{
	return av_rescale_q_rnd(val, from, to, AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX);
}

static void byteswap_s16(uint8_t *data, int size)
{
	for (int i = 0; i + 1 < size; i += 2) {
		uint8_t t = data[i];
		data[i] = data[i + 1];
		data[i + 1] = t;
	}
}

static void rtp_encoded_packet(void *data, struct encoder_packet *packet)
{
	struct rtp_output *out = data;
	if (!os_atomic_load_bool(&out->active) || !packet)
		return;

	if (rtp_stopping(out) && packet->sys_dts_usec >= (int64_t)out->stop_ts) {
		rtp_stop(out, 0);
		return;
	}

	bool is_video = packet->type == OBS_ENCODER_VIDEO;
	AVRational enc_tb = {packet->timebase_num, packet->timebase_den};
	AVRational dst_tb = is_video ? out->video.stream->time_base : out->audio.stream->time_base;

	AVPacket *pkt = av_packet_alloc();
	pkt->data = av_memdup(packet->data, packet->size);
	pkt->size = (int)packet->size;
	if (!pkt->data) {
		av_packet_free(&pkt);
		return;
	}

	if (!is_video && out->audio_is_le)
		byteswap_s16(pkt->data, pkt->size);

	pkt->stream_index = is_video ? 0 : 1; /* queue tag; write thread maps to session */
	pkt->pts = rescale_rtp(packet->pts, enc_tb, dst_tb);
	pkt->dts = rescale_rtp(packet->dts, enc_tb, dst_tb);
	if (packet->keyframe)
		pkt->flags |= AV_PKT_FLAG_KEY;

	pthread_mutex_lock(&out->write_mutex);
	da_push_back(out->packets, &pkt);
	pthread_mutex_unlock(&out->write_mutex);
	os_sem_post(out->write_sem);
}

static uint64_t rtp_total_bytes(void *data)
{
	struct rtp_output *out = data;
	return out->total_bytes;
}

struct obs_output_info ffmpeg_rtp_muxer = {
	.id = "ffmpeg_rtp_muxer",
	.flags = OBS_OUTPUT_AV | OBS_OUTPUT_ENCODED | OBS_OUTPUT_SERVICE,
	.protocols = "RTP",
	.encoded_video_codecs = "hevc",
	.encoded_audio_codecs = "pcm_s16le;pcm_s16be;pcm_bluray",
	.get_name = rtp_getname,
	.create = rtp_create,
	.destroy = rtp_destroy,
	.start = rtp_start,
	.stop = rtp_stop,
	.encoded_packet = rtp_encoded_packet,
	.get_total_bytes = rtp_total_bytes,
};
