/* ahb_mc.inc : AMediaCodec backend (byte-buffer mode) for the AHB VA-API driver.
 * Included by ahb_drv_video.c AFTER ahb_images.inc and BEFORE the decode pipeline.
 *
 * libmediandk is NOT linked: it is dlopen()ed from MEDIANDK_PATH at first use and all
 * functions are resolved with dlsym (needs -ldl). NDK headers are used only for types.
 *
 * Codecs: VP9, VP8 (frame passed through), H.264 (SPS/PPS synthesized + Annex B).
 * HEVC is NOT supported: VA-API does not carry the SPS short-term RPS sets, so the
 * slice headers cannot be made to match a synthesized SPS. AV1 is software on this device.
 *
 * Flow:
 *   create_context -> mc_open      (dlopen + createCodecByName + configure + start)
 *   end_picture    -> mc_packetize (under mu)  then  mc_submit (pts = target surface id)
 *   sync_surface   -> mc_sync      (drain output until the surface is written)
 *
 * Env:
 *   AHB_VA_SYNC_MS=500        max wait in vaSyncSurface
 *   AHB_VA_LAYOUT=i420        force planar output (default: from color-format; 21/flexible = NV12)
 *   AHB_VA_H264_LEVEL=51      level_idc written in the synthesized SPS
 *   AHB_VA_H264_REORDER=0     max_num_reorder_frames written in the SPS VUI (0 = output ASAP)
 *
 * Lock order: mc_mu -> mu. Never take mc_mu while holding mu.
 */

#include <dlfcn.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#include <stdint.h>
#include <time.h>

#ifndef MEDIANDK_PATH
#define MEDIANDK_PATH "/system/lib64/libmediandk.so"
#endif

/* ---- libmediandk function table ---- */
static struct {
    void* h;
    AMediaCodec*   (*createByName)(const char*);
    media_status_t (*configure)(AMediaCodec*, const AMediaFormat*, void* surface, void* crypto, uint32_t flags);
    media_status_t (*start)(AMediaCodec*);
    media_status_t (*stop)(AMediaCodec*);
    media_status_t (*del)(AMediaCodec*);
    ssize_t        (*dequeueIn)(AMediaCodec*, int64_t);
    uint8_t*       (*getIn)(AMediaCodec*, size_t, size_t*);
    media_status_t (*queueIn)(AMediaCodec*, size_t, off_t, size_t, uint64_t, uint32_t);
    ssize_t        (*dequeueOut)(AMediaCodec*, AMediaCodecBufferInfo*, int64_t);
    uint8_t*       (*getOut)(AMediaCodec*, size_t, size_t*);
    media_status_t (*releaseOut)(AMediaCodec*, size_t, bool);
    AMediaFormat*  (*getOutFormat)(AMediaCodec*);
    AMediaFormat*  (*fmtNew)(void);
    media_status_t (*fmtDelete)(AMediaFormat*);
    void           (*fmtSetString)(AMediaFormat*, const char*, const char*);
    void           (*fmtSetInt32)(AMediaFormat*, const char*, int32_t);
    bool           (*fmtGetInt32)(AMediaFormat*, const char*, int32_t*);
} mcapi;

static pthread_once_t mcapi_once = PTHREAD_ONCE_INIT;
static int mcapi_ok;

static void mcapi_load(void) {
    mcapi.h = dlopen(MEDIANDK_PATH, RTLD_NOW | RTLD_LOCAL);
    if (!mcapi.h) { LOG("dlopen(%s): %s\n", MEDIANDK_PATH, dlerror()); return; }
    int ok = 1;
#define LOADSYM(field, name) do { \
        *(void**)&mcapi.field = dlsym(mcapi.h, name); \
        if (!mcapi.field) { LOG("dlsym %s: %s\n", name, dlerror()); ok = 0; } } while (0)
    LOADSYM(createByName,  "AMediaCodec_createCodecByName");
    LOADSYM(configure,     "AMediaCodec_configure");
    LOADSYM(start,         "AMediaCodec_start");
    LOADSYM(stop,          "AMediaCodec_stop");
    LOADSYM(del,           "AMediaCodec_delete");
    LOADSYM(dequeueIn,     "AMediaCodec_dequeueInputBuffer");
    LOADSYM(getIn,         "AMediaCodec_getInputBuffer");
    LOADSYM(queueIn,       "AMediaCodec_queueInputBuffer");
    LOADSYM(dequeueOut,    "AMediaCodec_dequeueOutputBuffer");
    LOADSYM(getOut,        "AMediaCodec_getOutputBuffer");
    LOADSYM(releaseOut,    "AMediaCodec_releaseOutputBuffer");
    LOADSYM(getOutFormat,  "AMediaCodec_getOutputFormat");
    LOADSYM(fmtNew,        "AMediaFormat_new");
    LOADSYM(fmtDelete,     "AMediaFormat_delete");
    LOADSYM(fmtSetString,  "AMediaFormat_setString");
    LOADSYM(fmtSetInt32,   "AMediaFormat_setInt32");
    LOADSYM(fmtGetInt32,   "AMediaFormat_getInt32");
#undef LOADSYM
    mcapi_ok = ok;
    LOG("libmediandk loaded from %s: %s\n", MEDIANDK_PATH, ok ? "ok" : "INCOMPLETE");
}
static int mcapi_ready(void) {
    pthread_once(&mcapi_once, mcapi_load);
    return mcapi_ok;
}

/* ---- codec table ---- */
enum { MC_VP9 = 1, MC_VP8 = 2, MC_H264 = 3 };

static const struct { VAProfile p; const char* mime; const char* comp; int kind; } mc_tab[] = {
    { VAProfileVP9Profile0,              "video/x-vnd.on2.vp9", "c2.mtk.vp9.decoder", MC_VP9  },
    { VAProfileVP8Version0_3,            "video/x-vnd.on2.vp8", "c2.mtk.vpx.decoder", MC_VP8  },
    { VAProfileH264ConstrainedBaseline,  "video/avc",           "c2.mtk.avc.decoder", MC_H264 },
    { VAProfileH264Main,                 "video/avc",           "c2.mtk.avc.decoder", MC_H264 },
    { VAProfileH264High,                 "video/avc",           "c2.mtk.avc.decoder", MC_H264 },
};

static int mc_find(VAProfile p) {
    for (size_t i = 0; i < sizeof mc_tab / sizeof *mc_tab; i++) if (mc_tab[i].p == p) return (int)i;
    return -1;
}

typedef struct {
    int used, kind;
    AMediaCodec* codec;
    int w, h;                                  /* from vaCreateContext */
    int ofmt, ow, oh, ostride, oslice;         /* from getOutputFormat */
} mc_t;

static pthread_mutex_t mc_mu = PTHREAD_MUTEX_INITIALIZER;
static mc_t mcs[MAX_CTX];
static uint8_t surf_pend[MAX_SURF];            /* 1 = submitted, output not yet written */
static uint8_t surf_ctx[MAX_SURF];             /* owning context id (1..MAX_CTX) */
static int g_sync_ms = 500;
static int g_i420;
static int g_h264_level = 51;
static int g_h264_reorder = 0;

/* last SPS/PPS sent per context; protected by mu (touched only in end_picture/mc_open) */
static struct { uint8_t sps[96]; size_t sps_n; uint8_t pps[48]; size_t pps_n; } h264c[MAX_CTX];

#define PEND_GET(i)   __atomic_load_n(&surf_pend[i], __ATOMIC_ACQUIRE)
#define PEND_SET(i,v) __atomic_store_n(&surf_pend[i], (v), __ATOMIC_RELEASE)

static int64_t now_ms(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

/* called by destroy_surfaces (under mu): atomics only, no mc_mu */
static void mc_forget(unsigned id) {
    if (id >= 1 && id <= MAX_SURF) PEND_SET(id - 1, 0);
}

/* ======================================================================
 *  Packetizers (run in end_picture, with mu held)
 * ====================================================================== */

/* ---- growable byte buffer ---- */
typedef struct { uint8_t* p; size_t n, cap; } vbuf;
static int vb_put(vbuf* b, const void* d, size_t n) {
    if (b->n + n > b->cap) {
        size_t c = b->cap ? b->cap * 2 : 4096;
        while (c < b->n + n) c *= 2;
        uint8_t* t = realloc(b->p, c);
        if (!t) return 1;
        b->p = t; b->cap = c;
    }
    memcpy(b->p + b->n, d, n);
    b->n += n;
    return 0;
}
static void vb_free(vbuf* b) { free(b->p); memset(b, 0, sizeof *b); }

/* ---- show_frame detection: hidden VP8/VP9 frames produce no output ---- */
static int vp9_shown(const uint8_t* d, size_t n) {
    if (n < 2) return 1;
    if ((d[n - 1] & 0xe0) == 0xc0) return 1;          /* superframe index: assume shown */
    unsigned b = d[0];
    if ((b >> 6) != 2) return 1;                        /* bad frame_marker */
    int prof = ((b >> 5) & 1) | (((b >> 4) & 1) << 1);
    int pos = (prof == 3) ? 5 : 4;                      /* bit index of show_existing_frame */
    if ((b >> (7 - pos)) & 1) return 1;                 /* show_existing_frame */
    return (b >> (7 - (pos + 2))) & 1;                  /* show_frame */
}
static int vp8_shown(const uint8_t* d, size_t n) {
    return n < 3 ? 1 : (d[0] >> 4) & 1;
}

/* ---- bit writer for SPS/PPS (buffer must be zero-initialized) ---- */
typedef struct { uint8_t* p; size_t cap, bit; } bw_t;
static void bw_bit(bw_t* w, unsigned v) {
    if ((w->bit >> 3) < w->cap && v) w->p[w->bit >> 3] |= (uint8_t)(0x80 >> (w->bit & 7));
    w->bit++;
}
static void bw_bits(bw_t* w, unsigned v, int n) { for (int i = n - 1; i >= 0; i--) bw_bit(w, (v >> i) & 1); }
static void bw_ue(bw_t* w, unsigned v) {
    v++;
    int len = 0;
    for (unsigned t = v; t > 1; t >>= 1) len++;
    bw_bits(w, 0, len);
    bw_bits(w, v, len + 1);
}
static void bw_se(bw_t* w, int v) { bw_ue(w, v > 0 ? (unsigned)(2 * v - 1) : (unsigned)(-2 * v)); }

/* rbsp trailing bits + NAL header + emulation prevention; returns NAL size (without start code) */
static size_t bw_nal(bw_t* w, uint8_t hdr, uint8_t* out, size_t cap) {
    bw_bit(w, 1);
    size_t len = (w->bit + 7) >> 3;
    if (len > w->cap || cap < 2) return 0;
    size_t o = 0; int z = 0;
    out[o++] = hdr;
    for (size_t i = 0; i < len; i++) {
        uint8_t b = w->p[i];
        if (o + 2 > cap) return 0;
        if (z >= 2 && b <= 3) { out[o++] = 3; z = 0; }
        out[o++] = b;
        z = b ? 0 : z + 1;
    }
    return o;
}

/* ---- H.264: SPS from VAPictureParameterBufferH264 ----
 * Not available through VA-API, so fixed: level (env), no cropping (surfaces are coded size),
 * flat scaling lists, no POC type 1 offsets. VUI bitstream_restriction forces low output delay. */
static size_t h264_make_sps(const VAPictureParameterBufferH264* pp, VAProfile prof, uint8_t* out, size_t cap) {
    uint8_t rbsp[96] = {0};
    bw_t w = { rbsp, sizeof rbsp, 0 };
    int pidc = 100, cflags = 0;
    if (prof == VAProfileH264ConstrainedBaseline) { pidc = 66; cflags = 0xC0; }
    else if (prof == VAProfileH264Main)           { pidc = 77; }

    bw_bits(&w, (unsigned)pidc, 8);
    bw_bits(&w, (unsigned)cflags, 8);
    bw_bits(&w, (unsigned)g_h264_level, 8);
    bw_ue(&w, 0);                                           /* seq_parameter_set_id */
    if (pidc == 100) {
        unsigned cfi = pp->seq_fields.bits.chroma_format_idc;
        bw_ue(&w, cfi);
        if (cfi == 3) bw_bit(&w, 0);                        /* separate_colour_plane_flag */
        bw_ue(&w, pp->bit_depth_luma_minus8);
        bw_ue(&w, pp->bit_depth_chroma_minus8);
        bw_bit(&w, 0);                                      /* qpprime_y_zero_transform_bypass */
        bw_bit(&w, 0);                                      /* seq_scaling_matrix_present */
    }
    bw_ue(&w, pp->seq_fields.bits.log2_max_frame_num_minus4);
    unsigned poc = pp->seq_fields.bits.pic_order_cnt_type;
    bw_ue(&w, poc);
    if (poc == 0) bw_ue(&w, pp->seq_fields.bits.log2_max_pic_order_cnt_lsb_minus4);
    else if (poc == 1) {
        bw_bit(&w, pp->seq_fields.bits.delta_pic_order_always_zero_flag);
        bw_se(&w, 0); bw_se(&w, 0);                         /* offsets unknown */
        bw_ue(&w, 0);                                       /* num_ref_frames_in_pic_order_cnt_cycle */
    }
    bw_ue(&w, pp->num_ref_frames);
    bw_bit(&w, pp->seq_fields.bits.gaps_in_frame_num_value_allowed_flag);
    bw_ue(&w, pp->picture_width_in_mbs_minus1);
    unsigned mbh = (unsigned)pp->picture_height_in_mbs_minus1 + 1;
    unsigned fmo = pp->seq_fields.bits.frame_mbs_only_flag;
    bw_ue(&w, (fmo ? mbh : mbh / 2) - 1);                   /* pic_height_in_map_units_minus1 */
    bw_bit(&w, fmo);
    if (!fmo) bw_bit(&w, pp->seq_fields.bits.mb_adaptive_frame_field_flag);
    bw_bit(&w, pp->seq_fields.bits.direct_8x8_inference_flag);
    bw_bit(&w, 0);                                          /* frame_cropping_flag */

    bw_bit(&w, 1);                                          /* vui_parameters_present_flag */
    bw_bit(&w, 0);                                          /* aspect_ratio_info_present */
    bw_bit(&w, 0);                                          /* overscan_info_present */
    bw_bit(&w, 0);                                          /* video_signal_type_present */
    bw_bit(&w, 0);                                          /* chroma_loc_info_present */
    bw_bit(&w, 0);                                          /* timing_info_present */
    bw_bit(&w, 0);                                          /* nal_hrd_parameters_present */
    bw_bit(&w, 0);                                          /* vcl_hrd_parameters_present */
    bw_bit(&w, 0);                                          /* pic_struct_present */
    bw_bit(&w, 1);                                          /* bitstream_restriction_flag */
    bw_bit(&w, 1);                                          /* motion_vectors_over_pic_boundaries */
    bw_ue(&w, 0); bw_ue(&w, 0);                             /* max_bytes_per_pic / max_bits_per_mb denom */
    bw_ue(&w, 16); bw_ue(&w, 16);                           /* log2_max_mv_length h/v */
    unsigned reorder = (unsigned)g_h264_reorder;
    unsigned dpb = pp->num_ref_frames > reorder ? pp->num_ref_frames : reorder;
    if (dpb < 1) dpb = 1;
    if (dpb > 16) dpb = 16;
    bw_ue(&w, reorder);                                     /* max_num_reorder_frames */
    bw_ue(&w, dpb);                                         /* max_dec_frame_buffering */
    return bw_nal(&w, 0x67, out, cap);
}

/* ---- H.264: PPS. num_ref_idx defaults come from the slices (correct whenever the slice
 * header does not override them) ---- */
static size_t h264_make_pps(const VAPictureParameterBufferH264* pp, VAProfile prof, int l0, int l1,
        uint8_t* out, size_t cap) {
    uint8_t rbsp[48] = {0};
    bw_t w = { rbsp, sizeof rbsp, 0 };
    bw_ue(&w, 0);                                           /* pic_parameter_set_id */
    bw_ue(&w, 0);                                           /* seq_parameter_set_id */
    bw_bit(&w, pp->pic_fields.bits.entropy_coding_mode_flag);
    bw_bit(&w, pp->pic_fields.bits.pic_order_present_flag);
    bw_ue(&w, 0);                                           /* num_slice_groups_minus1 (no FMO) */
    bw_ue(&w, (unsigned)l0);
    bw_ue(&w, (unsigned)l1);
    bw_bit(&w, pp->pic_fields.bits.weighted_pred_flag);
    bw_bits(&w, pp->pic_fields.bits.weighted_bipred_idc, 2);
    bw_se(&w, pp->pic_init_qp_minus26);
    bw_se(&w, pp->pic_init_qs_minus26);
    bw_se(&w, pp->chroma_qp_index_offset);
    bw_bit(&w, pp->pic_fields.bits.deblocking_filter_control_present_flag);
    bw_bit(&w, pp->pic_fields.bits.constrained_intra_pred_flag);
    bw_bit(&w, pp->pic_fields.bits.redundant_pic_cnt_present_flag);
    int ext = prof == VAProfileH264High || pp->pic_fields.bits.transform_8x8_mode_flag ||
              pp->second_chroma_qp_index_offset != pp->chroma_qp_index_offset;
    if (ext) {
        bw_bit(&w, pp->pic_fields.bits.transform_8x8_mode_flag);
        bw_bit(&w, 0);                                      /* pic_scaling_matrix_present */
        bw_se(&w, pp->second_chroma_qp_index_offset);
    }
    return bw_nal(&w, 0x68, out, cap);
}

/* slice NAL (with its NAL header byte, emulation prevention intact) -> Annex B */
static int h264_emit_nal(vbuf* v, const uint8_t* d, size_t len, int* idr) {
    static const uint8_t sc[4] = { 0, 0, 0, 1 };
    if (len < 2 || (d[0] & 0x80)) { LOG("bad NAL header %02x\n", len ? d[0] : 0); return 1; }
    int t = d[0] & 0x1f;
    if (t != 1 && t != 5) { LOG("slice data does not start with a slice NAL header (%02x)\n", d[0]); return 1; }
    if (t == 5) *idr = 1;
    return vb_put(v, sc, 4) || vb_put(v, d, len);
}

static VAStatus h264_packetize(unsigned cid, VAProfile prof, const VABufferID* ids, int n, vbuf* out) {
    static const uint8_t sc[4] = { 0, 0, 0, 1 };
    static int warned_iq;
    const VAPictureParameterBufferH264* pp = NULL;
    int l0 = 0, l1 = 0, have0 = 0, have1 = 0, nslices = 0;

    for (int i = 0; i < n; i++) {                           /* pass 1: parameters */
        buf_t* b = buf_get(ids[i]);
        if (!b) continue;
        if (b->type == VAPictureParameterBufferType && b->size >= sizeof *pp) {
            pp = b->data;
        } else if (b->type == VASliceParameterBufferType && b->size >= sizeof(VASliceParameterBufferH264)) {
            for (unsigned k = 0; k < b->num; k++) {
                const VASliceParameterBufferH264* s =
                    (const void*)((const uint8_t*)b->data + (size_t)k * b->size);
                int st = s->slice_type % 5;                 /* 0 P, 1 B, 2 I, 3 SP, 4 SI */
                if (!have0 && st != 2 && st != 4) { l0 = s->num_ref_idx_l0_active_minus1; have0 = 1; }
                if (!have1 && st == 1)            { l1 = s->num_ref_idx_l1_active_minus1; have1 = 1; }
                nslices++;
            }
        } else if (b->type == VAIQMatrixBufferType && b->size >= sizeof(VAIQMatrixBufferH264) && !warned_iq) {
            const VAIQMatrixBufferH264* q = b->data;
            for (int k = 0; k < 96; k++) if (((const uint8_t*)q->ScalingList4x4)[k] != 16) warned_iq = 1;
            for (int k = 0; k < 128; k++) if (((const uint8_t*)q->ScalingList8x8)[k] != 16) warned_iq = 1;
            if (warned_iq) LOG("WARNING: stream uses custom scaling lists, not written to SPS/PPS\n");
        }
    }
    if (!pp || !nslices) return VA_STATUS_ERROR_DECODING_ERROR;

    vbuf body = {0};
    int idr = 0, bad = 0;
    const VASliceParameterBufferH264* pend = NULL;
    unsigned npend = 0; size_t pstride = 0;
    for (int i = 0; i < n && !bad; i++) {                   /* pass 2: slice NALs */
        buf_t* b = buf_get(ids[i]);
        if (!b) continue;
        if (b->type == VASliceParameterBufferType && b->size >= sizeof(VASliceParameterBufferH264)) {
            pend = b->data; npend = b->num; pstride = b->size;
        } else if (b->type == VASliceDataBufferType) {
            const uint8_t* d = b->data;
            size_t total = (size_t)b->size * b->num;
            if (npend) {
                for (unsigned k = 0; k < npend && !bad; k++) {
                    const VASliceParameterBufferH264* s =
                        (const void*)((const uint8_t*)pend + (size_t)k * pstride);
                    size_t off = s->slice_data_offset, len = s->slice_data_size;
                    if (off > total || len > total - off || h264_emit_nal(&body, d + off, len, &idr)) bad = 1;
                }
                npend = 0;
            } else if (h264_emit_nal(&body, d, total, &idr)) {
                bad = 1;
            }
        }
    }
    if (bad || !body.n) { vb_free(&body); return VA_STATUS_ERROR_DECODING_ERROR; }

    uint8_t sps[96], pps[48];
    size_t sn = h264_make_sps(pp, prof, sps, sizeof sps);
    size_t pn = h264_make_pps(pp, prof, l0, l1, pps, sizeof pps);
    if (!sn || !pn) { vb_free(&body); return VA_STATUS_ERROR_DECODING_ERROR; }

    int send_sps = idr || sn != h264c[cid - 1].sps_n || memcmp(sps, h264c[cid - 1].sps, sn);
    int send_pps = idr || pn != h264c[cid - 1].pps_n || memcmp(pps, h264c[cid - 1].pps, pn);
    int fail = 0;
    if (send_sps) fail |= vb_put(out, sc, 4) || vb_put(out, sps, sn);
    if (send_pps) fail |= vb_put(out, sc, 4) || vb_put(out, pps, pn);
    fail |= vb_put(out, body.p, body.n);
    vb_free(&body);
    if (fail) return VA_STATUS_ERROR_ALLOCATION_FAILED;

    memcpy(h264c[cid - 1].sps, sps, sn); h264c[cid - 1].sps_n = sn;
    memcpy(h264c[cid - 1].pps, pps, pn); h264c[cid - 1].pps_n = pn;
    LOG("h264: %d slice(s) idr=%d sps=%s pps=%s refs=%d/%d\n", nslices, idr,
        send_sps ? "sent" : "cached", send_pps ? "sent" : "cached", l0 + 1, l1 + 1);
    return VA_STATUS_SUCCESS;
}

/* ---- dispatch: render buffers -> one codec input buffer ---- */
static VAStatus mc_packetize(unsigned cid, VAProfile prof, const VABufferID* ids, int n,
        vbuf* out, int* shown) {
    int e = mc_find(prof);
    int kind = e < 0 ? 0 : mc_tab[e].kind;
    *shown = 1;
    if (cid < 1 || cid > MAX_CTX) return VA_STATUS_ERROR_INVALID_CONTEXT;
    if (kind == MC_H264) return h264_packetize(cid, prof, ids, n, out);
    if (kind != MC_VP9 && kind != MC_VP8) return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;

    for (int i = 0; i < n; i++) {                           /* VP8/VP9: whole frame = slice data */
        buf_t* b = buf_get(ids[i]);
        if (!b || b->type != VASliceDataBufferType) continue;
        size_t len = (size_t)b->size * b->num;
        if (len && vb_put(out, b->data, len)) return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }
    if (!out->n) return VA_STATUS_ERROR_DECODING_ERROR;
    *shown = (kind == MC_VP9) ? vp9_shown(out->p, out->n) : vp8_shown(out->p, out->n);
    return VA_STATUS_SUCCESS;
}

/* ======================================================================
 *  MediaCodec side
 * ====================================================================== */

/* ---- output format ---- */
static void mc_read_fmt(mc_t* m) {
    AMediaFormat* f = mcapi.getOutFormat(m->codec);
    if (!f) return;
    int32_t cf = 21, w = m->w, h = m->h, st = 0, sl = 0;
    mcapi.fmtGetInt32(f, "color-format", &cf);
    mcapi.fmtGetInt32(f, "width", &w);
    mcapi.fmtGetInt32(f, "height", &h);
    mcapi.fmtGetInt32(f, "stride", &st);
    mcapi.fmtGetInt32(f, "slice-height", &sl);
    if (st < w) st = w;
    if (sl < h) sl = h;
    m->ofmt = cf; m->ow = w; m->oh = h; m->ostride = st; m->oslice = sl;
    LOG("out format: color=0x%x %dx%d stride=%d slice=%d\n", cf, w, h, st, sl);
    mcapi.fmtDelete(f);
}

/* ---- copy a decoded frame (NV12 or I420) into the surface's AHB ---- */
static void mc_store(mc_t* m, const uint8_t* src, size_t sz, unsigned id) {
    AHardwareBuffer* ahb = NULL; unsigned sw = 0, sh = 0;
    pthread_mutex_lock(&mu);
    surf_t* s = surf_get(id);
    if (s) { ahb = s->ahb; sw = s->w; sh = s->h; AHardwareBuffer_acquire(ahb); }
    pthread_mutex_unlock(&mu);
    if (!ahb) return;

    unsigned w = (unsigned)m->ow < sw ? (unsigned)m->ow : sw;
    unsigned h = (unsigned)m->oh < sh ? (unsigned)m->oh : sh;
    size_t st = (size_t)m->ostride, sl = (size_t)m->oslice;
    int planar = g_i420 || m->ofmt == 19;
    unsigned cw = (w + 1) / 2, ch = (h + 1) / 2;
    size_t cst = planar ? st / 2 : st;
    size_t uoff = st * sl;
    size_t voff = uoff + cst * (sl / 2);
    size_t need = planar ? voff + (ch - 1) * cst + cw
                         : uoff + (ch - 1) * st + 2 * (size_t)cw;
    if (sz < need) {
        LOG("frame too small: have %zu need %zu (stride=%zu slice=%zu)\n", sz, need, st, sl);
        AHardwareBuffer_release(ahb);
        return;
    }

    AHardwareBuffer_Planes p;
    if (AHardwareBuffer_lockPlanes(ahb, AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN, -1, NULL, &p) == 0) {
        if (p.planeCount >= 3) {
            for (unsigned y = 0; y < h; y++)
                memcpy((uint8_t*)p.planes[0].data + (size_t)y * p.planes[0].rowStride, src + y * st, w);

            uint8_t* du = p.planes[1].data; uint8_t* dv = p.planes[2].data;
            size_t rs1 = p.planes[1].rowStride, rs2 = p.planes[2].rowStride;
            size_t ps1 = p.planes[1].pixelStride, ps2 = p.planes[2].pixelStride;

            if (!planar && ps1 == 2 && dv == du + 1) {            /* NV12 -> NV12 */
                for (unsigned cy = 0; cy < ch; cy++)
                    memcpy(du + cy * rs1, src + uoff + cy * st, (size_t)cw * 2);
            } else {                                              /* generic (NV21, I420, ...) */
                for (unsigned cy = 0; cy < ch; cy++) {
                    for (unsigned cx = 0; cx < cw; cx++) {
                        uint8_t u, v;
                        if (planar) { u = src[uoff + cy * cst + cx]; v = src[voff + cy * cst + cx]; }
                        else        { u = src[uoff + cy * st + 2 * cx]; v = src[uoff + cy * st + 2 * cx + 1]; }
                        du[cy * rs1 + cx * ps1] = u;
                        dv[cy * rs2 + cx * ps2] = v;
                    }
                }
            }
        }
        AHardwareBuffer_unlock(ahb, NULL);
    }
    AHardwareBuffer_release(ahb);
}

/* ---- drain output (caller holds mc_mu). First wait = to_us, then only what is ready ---- */
static void mc_drain(mc_t* m, int64_t to_us) {
    unsigned cid = (unsigned)(m - mcs) + 1;
    for (;;) {
        AMediaCodecBufferInfo bi;
        ssize_t o = mcapi.dequeueOut(m->codec, &bi, to_us);
        to_us = 0;
        if (o >= 0) {
            unsigned id = (unsigned)bi.presentationTimeUs;
            if (id >= 1 && id <= MAX_SURF && PEND_GET(id - 1) && surf_ctx[id - 1] == cid) {
                size_t cap = 0;
                uint8_t* p = mcapi.getOut(m->codec, (size_t)o, &cap);
                if (!m->ow) mc_read_fmt(m);
                if (p && cap > (size_t)bi.offset) mc_store(m, p + bi.offset, cap - bi.offset, id);
                PEND_SET(id - 1, 0);
            } else {
                LOG("output pts=%lld ignored (not pending)\n", (long long)bi.presentationTimeUs);
            }
            mcapi.releaseOut(m->codec, (size_t)o, false);
        } else if (o == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
            mc_read_fmt(m);
        } else if (o == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) {
            /* nothing: we use getOutputBuffer(idx) */
        } else {
            break;   /* TRY_AGAIN_LATER or error */
        }
    }
}

/* ---- open / close ---- */
static VAStatus mc_open(unsigned cid, VAProfile prof, int w, int h) {
    int e = mc_find(prof);
    if (e < 0) { LOG("mc_open: profile %d has no backend\n", prof); return VA_STATUS_ERROR_UNSUPPORTED_PROFILE; }
    if (cid < 1 || cid > MAX_CTX || w <= 0 || h <= 0) return VA_STATUS_ERROR_INVALID_PARAMETER;
    if (!mcapi_ready()) return VA_STATUS_ERROR_OPERATION_FAILED;

    const char* ev = getenv("AHB_VA_SYNC_MS");     if (ev && atoi(ev) > 0) g_sync_ms = atoi(ev);
    ev = getenv("AHB_VA_LAYOUT");                  g_i420 = ev && !strcmp(ev, "i420");
    ev = getenv("AHB_VA_H264_LEVEL");              if (ev && atoi(ev) > 0) g_h264_level = atoi(ev);
    ev = getenv("AHB_VA_H264_REORDER");            if (ev && atoi(ev) >= 0) g_h264_reorder = atoi(ev);

    pthread_mutex_lock(&mu);
    memset(&h264c[cid - 1], 0, sizeof h264c[0]);
    pthread_mutex_unlock(&mu);

    AMediaCodec* c = mcapi.createByName(mc_tab[e].comp);
    if (!c) { LOG("createCodecByName(%s) failed\n", mc_tab[e].comp); return VA_STATUS_ERROR_OPERATION_FAILED; }

    AMediaFormat* f = mcapi.fmtNew();
    int32_t maxin = (int32_t)((size_t)w * h * 3 / 2);
    if (maxin < (1 << 20)) maxin = 1 << 20;
    mcapi.fmtSetString(f, "mime", mc_tab[e].mime);
    mcapi.fmtSetInt32(f, "width", w);
    mcapi.fmtSetInt32(f, "height", h);
    mcapi.fmtSetInt32(f, "max-input-size", maxin);
    mcapi.fmtSetInt32(f, "low-latency", 1);                      /* API 30 */
    media_status_t st = mcapi.configure(c, f, NULL, NULL, 0);
    mcapi.fmtDelete(f);
    if (st == AMEDIA_OK) st = mcapi.start(c);
    if (st != AMEDIA_OK) {
        LOG("codec %s configure/start failed: %d\n", mc_tab[e].comp, (int)st);
        mcapi.del(c);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    pthread_mutex_lock(&mc_mu);
    mcs[cid - 1] = (mc_t){ .used = 1, .kind = mc_tab[e].kind, .codec = c, .w = w, .h = h };
    pthread_mutex_unlock(&mc_mu);
    LOG("mc_open ctx=%u %s %dx%d\n", cid, mc_tab[e].comp, w, h);
    return VA_STATUS_SUCCESS;
}

static void mc_close(unsigned cid) {
    if (cid < 1 || cid > MAX_CTX) return;
    pthread_mutex_lock(&mc_mu);
    mc_t* m = &mcs[cid - 1];
    if (m->used) {
        mcapi.stop(m->codec);
        mcapi.del(m->codec);
        for (int i = 0; i < MAX_SURF; i++) if (PEND_GET(i) && surf_ctx[i] == cid) PEND_SET(i, 0);
        memset(m, 0, sizeof *m);
    }
    pthread_mutex_unlock(&mc_mu);
}

/* ---- submit one access unit (already packetized) ---- */
static VAStatus mc_submit(unsigned cid, unsigned target, const uint8_t* d, size_t n, int shown) {
    VAStatus r = VA_STATUS_SUCCESS;
    pthread_mutex_lock(&mc_mu);
    mc_t* m = (cid >= 1 && cid <= MAX_CTX && mcs[cid - 1].used) ? &mcs[cid - 1] : NULL;
    if (!m) r = VA_STATUS_ERROR_INVALID_CONTEXT;
    else if (!n || !d || target < 1 || target > MAX_SURF) r = VA_STATUS_ERROR_DECODING_ERROR;
    else {
        LOG("submit ctx=%u target=%u %zu bytes shown=%d head=%02x %02x %02x %02x %02x\n", cid, target, n, shown,
            d[0], n > 1 ? d[1] : 0, n > 2 ? d[2] : 0, n > 3 ? d[3] : 0, n > 4 ? d[4] : 0);
        ssize_t i = -1;
        for (int t = 0; t < 200 && i < 0; t++) {
            i = mcapi.dequeueIn(m->codec, 5000);
            if (i < 0) mc_drain(m, 0);      /* free output buffers so the codec can progress */
        }
        if (i < 0) r = VA_STATUS_ERROR_OPERATION_FAILED;
        else {
            size_t cap = 0;
            uint8_t* in = mcapi.getIn(m->codec, (size_t)i, &cap);
            if (!in || n > cap) {
                LOG("input too big: %zu > %zu\n", n, cap);
                mcapi.queueIn(m->codec, (size_t)i, 0, 0, 0, 0);
                r = VA_STATUS_ERROR_DECODING_ERROR;
            } else {
                memcpy(in, d, n);
                if (!shown) PEND_SET(target - 1, 0);
                else { PEND_SET(target - 1, 1); surf_ctx[target - 1] = (uint8_t)cid; }
                if (mcapi.queueIn(m->codec, (size_t)i, 0, n, target, 0) != AMEDIA_OK) {
                    PEND_SET(target - 1, 0);
                    r = VA_STATUS_ERROR_OPERATION_FAILED;
                } else {
                    mc_drain(m, 0);
                }
            }
        }
    }
    pthread_mutex_unlock(&mc_mu);
    return r;
}

/* ---- sync: wait until the output for surface s has been written ---- */
static VAStatus mc_sync(VASurfaceID s) {
    if (s < 1 || s > MAX_SURF) return VA_STATUS_ERROR_INVALID_SURFACE;
    int64_t deadline = now_ms() + g_sync_ms;
    for (;;) {
        VAStatus r = VA_STATUS_SUCCESS;
        int done = 1;
        pthread_mutex_lock(&mc_mu);
        if (PEND_GET(s - 1)) {
            unsigned cid = surf_ctx[s - 1];
            mc_t* m = (cid >= 1 && cid <= MAX_CTX && mcs[cid - 1].used) ? &mcs[cid - 1] : NULL;
            if (!m) { PEND_SET(s - 1, 0); r = VA_STATUS_ERROR_INVALID_CONTEXT; }
            else {
                mc_drain(m, 5000);
                if (PEND_GET(s - 1)) {
                    if (now_ms() >= deadline) {
                        LOG("sync timeout surface=%u\n", s);
                        PEND_SET(s - 1, 0);
                        r = VA_STATUS_ERROR_DECODING_ERROR;
                    } else done = 0;
                }
            }
        }
        pthread_mutex_unlock(&mc_mu);
        if (done) return r;
    }
}

/* replace the old stubs from the surfaces section */
static VAStatus sync_surface(VADriverContextP c, VASurfaceID s) {
    (void)c;
    return mc_sync(s);
}
static VAStatus q_surface_status(VADriverContextP c, VASurfaceID s, VASurfaceStatus* st) {
    (void)c;
    int pend = 0;
    if (s >= 1 && s <= MAX_SURF) pend = PEND_GET(s - 1);
    *st = pend ? VASurfaceRendering : VASurfaceReady;
    return VA_STATUS_SUCCESS;
}
