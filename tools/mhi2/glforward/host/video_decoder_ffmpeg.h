/* libavcodec backend. Device/format negotiation follows FFmpeg's public
 * hw_decode API. The wire parser, crop and display routing are host-neutral. */
static void video_end_session(struct VideoDecoder *v)
{
    avcodec_free_context(&v->codec);
    sws_freeContext(v->scaler);
    v->scaler = NULL;
    v->hardware = false;
    v->session_frames = 0;
}

static enum AVPixelFormat video_hw_format(AVCodecContext *ctx,
                                         const enum AVPixelFormat *formats)
{
    struct VideoDecoder *v = ctx->opaque;
    for (; *formats != AV_PIX_FMT_NONE; formats++) {
        if (*formats == v->hw_format) {
            return *formats;
        }
    }
    /* Do not let libavcodec silently select a CPU format for a HW session. */
    return AV_PIX_FMT_NONE;
}

static int video_start_session(struct VideoDecoder *v, bool software)
{
    const AVCodec *decoder = avcodec_find_decoder_by_name("h264");
    if (!decoder) {
        decoder = avcodec_find_decoder(AV_CODEC_ID_H264);
    }
    video_end_session(v);
    if (!decoder || !(v->codec = avcodec_alloc_context3(decoder))) {
        return -1;
    }
    AVCodecContext *ctx = v->codec;
    ctx->width = v->width;
    ctx->height = v->height;
    ctx->max_pixels = 2048 * 2048;
    ctx->thread_count = 1;
    ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
    ctx->err_recognition = AV_EF_EXPLODE;
    ctx->opaque = v;
    ctx->extradata_size = v->sps_size + v->pps_size + 8;
    ctx->extradata = av_mallocz(ctx->extradata_size + AV_INPUT_BUFFER_PADDING_SIZE);
    if (!ctx->extradata) {
        goto fail;
    }
    memcpy(ctx->extradata, "\0\0\0\1", 4);
    memcpy(ctx->extradata + 4, v->sps, v->sps_size);
    memcpy(ctx->extradata + 4 + v->sps_size, "\0\0\0\1", 4);
    memcpy(ctx->extradata + 8 + v->sps_size, v->pps, v->pps_size);
    v->hw_format = AV_PIX_FMT_NONE;
    if (!software && !v->software_only) {
#ifdef __APPLE__
        enum AVHWDeviceType type = AV_HWDEVICE_TYPE_VIDEOTOOLBOX;
#else
        enum AVHWDeviceType type = AV_HWDEVICE_TYPE_VAAPI;
#endif
        const char *device = getenv("MHI2_DECODE_DEVICE");
        for (unsigned i = 0; ; i++) {
            const AVCodecHWConfig *config = avcodec_get_hw_config(decoder, i);
            if (!config) {
                break;
            }
            if (config->device_type == type &&
                (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) &&
                av_hwdevice_ctx_create(&ctx->hw_device_ctx, type, device, NULL, 0) >= 0) {
                v->hw_format = config->pix_fmt;
                ctx->get_format = video_hw_format;
                break;
            }
        }
    }
    if (v->hw_format == AV_PIX_FMT_NONE && v->require_hardware) {
        fprintf(stderr, "videobridge: requested hardware decoder unavailable\n");
        goto fail;
    }
    if (avcodec_open2(ctx, decoder, NULL) < 0) {
        goto fail;
    }
    fprintf(stderr, "videobridge: %s session owner=%u id=%u (%s)\n",
            v->hw_format == AV_PIX_FMT_NONE ? "software" : "hardware",
            v->owner, v->id, decoder->name);
    return 0;
fail:
    video_end_session(v);
    return -1;
}

static int video_publish(struct VideoDecoder *v, AVFrame *frame)
{
    AVFrame *cpu = NULL;
    bool hardware = frame->format == v->hw_format && v->hw_format != AV_PIX_FMT_NONE;
    int result = -1;
    /* Check dimensions before a transfer or output allocation. */
    if (frame->width != v->width || frame->height != v->height) {
        return -1;
    }
    if (hardware) {
        cpu = av_frame_alloc();
        if (!cpu || av_hwframe_transfer_data(cpu, frame, 0) < 0 ||
            av_frame_copy_props(cpu, frame) < 0) {
            goto done;
        }
        frame = cpu;
    } else if (v->require_hardware) {
        goto done;
    }
    if (!v->rgba) {
        v->rgba = malloc((size_t)v->width * v->height * 4);
        if (!v->rgba) {
            goto done;
        }
    }
    v->scaler = sws_getCachedContext(v->scaler, v->width, v->height, frame->format,
                                    v->width, v->height, AV_PIX_FMT_RGBA,
                                    SWS_BILINEAR, NULL, NULL, NULL);
    if (!v->scaler) {
        goto done;
    }
    int matrix = SWS_CS_DEFAULT;
    switch (frame->colorspace) {
    case AVCOL_SPC_BT709: matrix = SWS_CS_ITU709; break;
    case AVCOL_SPC_FCC: matrix = SWS_CS_FCC; break;
    case AVCOL_SPC_SMPTE240M: matrix = SWS_CS_SMPTE240M; break;
    case AVCOL_SPC_BT2020_NCL: matrix = SWS_CS_BT2020; break;
    default: break;
    }
    const int *coefficients = sws_getCoefficients(matrix);
    if (sws_setColorspaceDetails(v->scaler, coefficients,
                                 frame->color_range == AVCOL_RANGE_JPEG,
                                 coefficients, 1, 0, 1 << 16, 1 << 16) < 0) {
        goto done;
    }
    uint8_t *planes[4] = {v->rgba};
    int strides[4] = {v->width * 4};
    if (sws_scale(v->scaler, (const uint8_t *const *)frame->data, frame->linesize,
                  0, v->height, planes, strides) != v->height) {
        goto done;
    }
    v->hardware = hardware;
    v->session_frames++;
    if (++v->frames == 1 || v->frames % 300 == 0) {
        fprintf(stderr, "videobridge: decoded %u frames hardware=%d\n", v->frames, hardware);
    }
    video_display_changed();
    result = 0;
done:
    av_frame_free(&cpu);
    return result;
}

static int video_receive(struct VideoDecoder *v)
{
    AVFrame *frame = av_frame_alloc();
    if (!frame) {
        return -1;
    }
    int result;
    while ((result = avcodec_receive_frame(v->codec, frame)) >= 0) {
        result = video_publish(v, frame);
        av_frame_unref(frame);
        if (result < 0) {
            break;
        }
    }
    av_frame_free(&frame);
    return result == AVERROR(EAGAIN) || result == AVERROR_EOF ? 0 : result;
}

static int video_submit(struct VideoDecoder *v, const uint8_t *data, size_t size)
{
    AVPacket *packet = av_packet_alloc();
    if (!packet || av_new_packet(packet, size) < 0) {
        av_packet_free(&packet);
        return -1;
    }
    memcpy(packet->data, data, size);
    int result = avcodec_send_packet(v->codec, packet);
    if (result >= 0) {
        result = video_receive(v);
    }
    /* A device may exist but reject this codec/profile. Auto mode retries the
     * first access unit in software; explicit hardware requests never do. */
    if (result < 0 && !v->session_frames && !v->require_hardware &&
        v->hw_format != AV_PIX_FMT_NONE && !video_start_session(v, true)) {
        fprintf(stderr, "videobridge: hardware rejected stream; software fallback\n");
        result = avcodec_send_packet(v->codec, packet);
        if (result >= 0) {
            result = video_receive(v);
        }
    }
    av_packet_free(&packet);
    return result;
}
