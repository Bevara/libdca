/*
 *			GPAC - Multimedia Framework C SDK
 *
 *  This file is part of GPAC / DTS decoder filter, based on dcadec
 *  (https://github.com/foo86/dcadec) - a standalone DTS Coherent Acoustics
 *  decoder, no ffmpeg involved.
 *
 *  Elementary .dts streams are a sequence of core frames; the frame splitter
 *  below handles the 16-bit big endian sync word (0x7FFE8001), which is what
 *  every .dts file in the wild uses. Extension substreams (DTS-HD) are parsed
 *  by dcadec itself when they follow a core frame.
 */

#include <gpac/filters.h>
#include <gpac/constants.h>
#include <string.h>
#include <stdlib.h>

#include <libdcadec/dca_context.h>

#define DCA_SYNC_WORD 0x7FFE8001u

typedef struct
{
	GF_FilterPid *ipid, *opid;
	Bool is_playing;
} GF_DCADecCtx;

/* Returns the length of the core frame starting at p, 0 if there is no sync
 * word there. FSIZE sits at bits 14..27 of the word following the sync and
 * holds the frame length minus one. */
static u32 dcadec_frame_size(const u8 *p, u32 avail)
{
	u32 w;
	if (avail < 8)
		return 0;
	if (((u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]) != DCA_SYNC_WORD)
		return 0;
	w = (u32)p[4] << 24 | (u32)p[5] << 16 | (u32)p[6] << 8 | p[7];
	return ((w >> 4) & 0x3FFF) + 1;
}

static GF_Err dcadec_configure_pid(GF_Filter *filter, GF_FilterPid *pid, Bool is_remove)
{
	GF_DCADecCtx *ctx = (GF_DCADecCtx *)gf_filter_get_udta(filter);

	if (is_remove)
	{
		if (ctx->opid)
		{
			gf_filter_pid_remove(ctx->opid);
			ctx->opid = NULL;
		}
		ctx->ipid = NULL;
		return GF_OK;
	}
	if (!gf_filter_pid_check_caps(pid))
		return GF_NOT_SUPPORTED;

	ctx->ipid = pid;
	gf_filter_pid_set_framing_mode(pid, GF_TRUE);

	if (!ctx->opid)
		ctx->opid = gf_filter_pid_new(filter);

	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_STREAM_TYPE, &PROP_UINT(GF_STREAM_AUDIO));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_CODECID, &PROP_UINT(GF_CODECID_RAW));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_AUDIO_FORMAT, &PROP_UINT(GF_AUDIO_FMT_S16));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_SAMPLE_RATE, &PROP_UINT(48000));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_TIMESCALE, &PROP_UINT(48000));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_NUM_CHANNELS, &PROP_UINT(2));

	return GF_OK;
}

static Bool dcadec_process_event(GF_Filter *filter, const GF_FilterEvent *evt)
{
	GF_DCADecCtx *ctx = (GF_DCADecCtx *)gf_filter_get_udta(filter);
	switch (evt->base.type)
	{
	case GF_FEVT_PLAY:
		ctx->is_playing = GF_TRUE;
		return GF_FALSE;
	case GF_FEVT_STOP:
		ctx->is_playing = GF_FALSE;
		return GF_FALSE;
	default:
		return GF_FALSE;
	}
}

static GF_Err dcadec_process(GF_Filter *filter)
{
	GF_FilterPacket *pck, *dst_pck;
	u8 *data, *output;
	u32 size, pos, pcm_used = 0, pcm_alloc, channels = 0, out_rate = 0;
	struct dcadec_context *dca;
	s16 *pcm = NULL;
	GF_DCADecCtx *ctx = (GF_DCADecCtx *)gf_filter_get_udta(filter);

	pck = gf_filter_pid_get_packet(ctx->ipid);
	if (!pck)
	{
		if (gf_filter_pid_is_eos(ctx->ipid))
		{
			gf_filter_pid_set_eos(ctx->opid);
			return GF_EOS;
		}
		return GF_OK;
	}
	data = (u8 *)gf_filter_pck_get_data(pck, &size);
	if (!data)
	{
		gf_filter_pid_drop_packet(ctx->ipid);
		return GF_IO_ERR;
	}

	/* Skip anything before the first sync word (ID3 tags, garbage). */
	for (pos = 0; pos + 8 <= size; pos++)
	{
		if (dcadec_frame_size(data + pos, size - pos))
			break;
	}
	if (pos + 8 > size)
	{
		gf_filter_pid_drop_packet(ctx->ipid);
		GF_LOG(GF_LOG_ERROR, GF_LOG_CODEC, ("[DCADec] No DTS sync word found\n"));
		return GF_NON_COMPLIANT_BITSTREAM;
	}

	dca = dcadec_context_create(0);
	if (!dca)
	{
		gf_filter_pid_drop_packet(ctx->ipid);
		return GF_OUT_OF_MEM;
	}

	pcm_alloc = 48000 * 2; /* one second of stereo, grown as needed */
	pcm = (s16 *)gf_malloc(pcm_alloc * sizeof(s16));

	while (pcm && (pos + 8 <= size))
	{
		u32 frame_size = dcadec_frame_size(data + pos, size - pos);
		int **samples = NULL;
		int nsamples = 0, channel_mask = 0, sample_rate = 0, bits_per_sample = 0, profile = 0;
		int nb_ch, ch, i, shift;

		if (!frame_size || (pos + frame_size > size))
			break;
		if (dcadec_context_parse(dca, data + pos, frame_size) < 0)
			break;
		pos += frame_size;

		if (dcadec_context_filter(dca, &samples, &nsamples, &channel_mask,
								  &sample_rate, &bits_per_sample, &profile) < 0)
			break;
		if (!samples || (nsamples <= 0))
			continue;

		for (nb_ch = 0, i = 0; i < 32; i++)
			if (channel_mask & (1 << i))
				nb_ch++;
		if (!nb_ch)
			continue;
		channels = (u32)nb_ch;
		out_rate = (u32)sample_rate;
		/* dcadec hands back planar samples scaled to bits_per_sample. */
		shift = (bits_per_sample > 16) ? (bits_per_sample - 16) : 0;

		if (pcm_used + (u32)nsamples * channels > pcm_alloc)
		{
			s16 *bigger;
			while (pcm_used + (u32)nsamples * channels > pcm_alloc)
				pcm_alloc *= 2;
			bigger = (s16 *)gf_realloc(pcm, pcm_alloc * sizeof(s16));
			if (!bigger)
				break;
			pcm = bigger;
		}
		for (i = 0; i < nsamples; i++)
		{
			for (ch = 0; ch < nb_ch; ch++)
			{
				int v = shift ? (samples[ch][i] >> shift) : samples[ch][i];
				if (v > 32767) v = 32767;
				else if (v < -32768) v = -32768;
				pcm[pcm_used++] = (s16)v;
			}
		}
	}

	dcadec_context_destroy(dca);
	gf_filter_pid_drop_packet(ctx->ipid);

	if (!pcm || !pcm_used || !channels || !out_rate)
	{
		if (pcm)
			gf_free(pcm);
		GF_LOG(GF_LOG_ERROR, GF_LOG_CODEC, ("[DCADec] Stream decoded to no audio\n"));
		return GF_NON_COMPLIANT_BITSTREAM;
	}

	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_SAMPLE_RATE, &PROP_UINT(out_rate));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_TIMESCALE, &PROP_UINT(out_rate));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_NUM_CHANNELS, &PROP_UINT(channels));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_CHANNEL_LAYOUT, &PROP_LONGUINT((channels == 1) ? GF_AUDIO_CH_FRONT_CENTER : (GF_AUDIO_CH_FRONT_LEFT | GF_AUDIO_CH_FRONT_RIGHT)));

	dst_pck = gf_filter_pck_new_alloc(ctx->opid, pcm_used * (u32)sizeof(s16), &output);
	if (!dst_pck)
	{
		gf_free(pcm);
		return GF_OUT_OF_MEM;
	}
	memcpy(output, pcm, pcm_used * sizeof(s16));
	gf_free(pcm);

	gf_filter_pck_set_cts(dst_pck, 0);
	gf_filter_pck_set_sap(dst_pck, GF_FILTER_SAP_1);
	gf_filter_pck_set_duration(dst_pck, pcm_used / channels);
	gf_filter_pck_send(dst_pck);

	gf_filter_pid_set_eos(ctx->opid);
	return GF_EOS;
}

static void dcadec_finalize(GF_Filter *filter)
{
}

static const GF_FilterCapability DCADecCaps[] =
	{
		CAP_UINT(GF_CAPS_INPUT, GF_PROP_PID_STREAM_TYPE, GF_STREAM_FILE),
		CAP_STRING(GF_CAPS_INPUT, GF_PROP_PID_FILE_EXT, "dts|dtshd|cpt"),
		CAP_STRING(GF_CAPS_INPUT, GF_PROP_PID_MIME, "audio/vnd.dts|audio/x-dts"),
		CAP_UINT(GF_CAPS_OUTPUT, GF_PROP_PID_STREAM_TYPE, GF_STREAM_AUDIO),
		CAP_UINT(GF_CAPS_OUTPUT, GF_PROP_PID_CODECID, GF_CODECID_RAW),
};

GF_FilterRegister DCADecoderRegister = {
	.name = "dcadec",
	GF_FS_SET_DESCRIPTION("DTS Coherent Acoustics decoder")
		GF_FS_SET_HELP("This filter decodes DTS elementary streams using dcadec, a standalone DTS decoder.")
			.private_size = sizeof(GF_DCADecCtx),
	SETCAPS(DCADecCaps),
	.configure_pid = dcadec_configure_pid,
	.process = dcadec_process,
	.process_event = dcadec_process_event,
	.finalize = dcadec_finalize,
};

const GF_FilterRegister *EMSCRIPTEN_KEEPALIVE dcadec_register(GF_FilterSession *session)
{
	return &DCADecoderRegister;
}

#include "filter_register.h"
__attribute__((constructor))
void register_dcadec(void) {
    gf_filter_auto_register("dcadec", dcadec_register);
}
