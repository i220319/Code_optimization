void *player_thread(play_para_t *player)
{
    am_packet_t am_pkt;
    AVPacket avpkt;
    am_packet_t *pkt = NULL;
    int ret;
    unsigned int exit_flag = 0;
    pkt = &am_pkt;
    player_file_type_t filetype;
    int no_buffering_start = 0;
    //#define SAVE_YUV_FILE

#ifdef SAVE_YUV_FILE
    int out_fp = -1;
#endif

    AVCodecContext *ic = NULL;
    AVCodec *codec = NULL;
    AVFrame *picture = NULL;
    int got_picture = 0;
    int AFORMAT_SW_Flag = 0;
    char *audio_out_buf = NULL;
    int  audio_out_size = 0;
    int fffb_number = 0;
    int force_disable_cmcc_report = 0;
    int h264_error_recovery_mode = -1;
    int mh264_error_proc_policy = -1;
    int nal_skip_policy_old = amsysfs_get_sysfs_int("/sys/module/amvdec_h265/parameters/nal_skip_policy");
    int mpeg12_error_skip_level = 1;

    int64_t last_trick_toggle = -1;
    log_print("\npid[%d]::enter into player_thread\n", player->player_id);
    player->seek_async = am_getconfig_int_def("media.amplayer.seek_async", 0);

    update_player_start_paras(player, player->start_param);
    player_para_init(player);
    av_packet_init(pkt);
    avpkt_cache_init(player);

    pkt->avpkt = &avpkt;
    av_init_packet(pkt->avpkt);
    player->p_pkt = pkt;

    //player_thread_wait(player, 100 * 1000);    //wait pid send finish
    set_player_state(player, PLAYER_INITING);
    update_playing_info(player);
    update_player_states(player, 1);
    int64_t init_start = av_gettime();
    /*start open file and get file type*/
    ret = ffmpeg_open_file(player);
    if (ret != FFMPEG_SUCCESS) {
        int ecode = 0;
        if(ret == AVERROR_INVALIDDATA && (ecode = am_getconfig_int_def("media.player.errorcode1", 0)) > 0){
            //set_player_error_no(player, 54004);
            send_event(player,PLAYER_EVENTS_ERROR, ecode, 0);
            set_player_state(player, PLAYER_ERROR);
        }else if(am_getconfig_bool_def("media.player.cmcc_report.enable", 0)) {
            if (am_getconfig_bool_def("media.player.cmcc_shandong", 0)
                && strstr(player->file_name, "http://")
                && (ret == -404)
                && strstr(player->file_name, ".m3u8")) {
                send_event(player,PLAYER_EVENTS_ERROR, 10001, -ret);
            } else if (am_getconfig_bool_def("media.player.cmcc_hubei", 0) &&
                    (ret == -403 || ret == -404)) {
                log_print("[%s:%d] ret:%d\n", __FUNCTION__, __LINE__, ret);
                int errorMessage = ret == -403 ? MEDIA_INFO_HTTP_CONNECT_ERROR_403 : MEDIA_INFO_HTTP_CONNECT_ERROR_404;
                send_event(player, errorMessage, -ret, 0);
            } else {
                send_event(player,PLAYER_EVENTS_ERROR, 10001, 0);
            }
            set_player_state(player, PLAYER_ERROR);
        } else{
            set_player_state(player, PLAYER_ERROR);
            send_event(player, PLAYER_EVENTS_ERROR, ret, "Open File failed");
        }
        log_print("[player_dec_init]ffmpeg_open_file failed(%s)*****ret=%x!\n", player->file_name, ret);
        goto release0;
    }
    if (player->pFormatCtx && player->pFormatCtx->pb && player->pFormatCtx->pb->is_slowmedia) {
        url_set_seek_flags(player->pFormatCtx->pb, LESS_BUFF_DATA | NO_READ_RETRY);
    }


	if (am_getconfig_bool_def("media.player.cmcc_report.enable", 0)) {
        if ( player->pFormatCtx->pb && player->pFormatCtx->pb->local_playback == 1) {
            force_disable_cmcc_report = 1;
            property_set("media.player.cmcc_report.enable", "0");
	        player->cmcc_report_enable = 0;
        } else if (player->pFormatCtx != NULL
            && player->pFormatCtx->pb != NULL
            && player->pFormatCtx->pb->opaque != NULL){
            URLContext *h = (URLContext *)player->pFormatCtx->pb->opaque;
            if (h != NULL && h->prot != NULL
                && (strncmp(h->prot->name, "rtp", 3) == 0
                    || strcmp(h->prot->name, "udp") == 0) && am_getconfig_bool_def("media.player.blurdisplay.enable", 1)) {
                player->error_recovery_mode = 1;
                h264_error_recovery_mode = amsysfs_get_sysfs_int("/sys/module/amvdec_h264/parameters/error_recovery_mode");
                if (h264_error_recovery_mode != 0) {
                    amsysfs_set_sysfs_int("/sys/module/amvdec_h264/parameters/error_recovery_mode", 0);
                }

                int bad_block_scale = am_getconfig_int_def("media.libplayer.bad_block_scale", 30);
                log_print("error_recovery_mode = %d, bad_block_scale = %d\n", h264_error_recovery_mode, bad_block_scale);
                amsysfs_set_sysfs_int("/sys/module/amvdec_h264/parameters/bad_block_scale", bad_block_scale);
                mh264_error_proc_policy = amsysfs_get_sysfs_int("/sys/module/amvdec_mh264/parameters/error_proc_policy");
                if (mh264_error_proc_policy != 0x8077C3B7) {
                    amsysfs_set_sysfs_int16("/sys/module/amvdec_mh264/parameters/error_proc_policy", 0x8077C3B7);
                }
                // mpeg12
                mpeg12_error_skip_level = amsysfs_get_sysfs_int("/sys/module/amvdec_mpeg12/parameters/error_frame_skip_level");
                amsysfs_set_sysfs_int("/sys/module/amvdec_mpeg12/parameters/error_frame_skip_level", 0);
            }
        }
    }

    ffmpeg_parse_file_type(player, &filetype);
    set_player_state(player, PLAYER_TYPE_REDY);
    send_event(player, PLAYER_EVENTS_STATE_CHANGED, PLAYER_TYPE_REDY, 0);
    send_event(player, PLAYER_EVENTS_FILE_TYPE, &filetype, 0);
    if (player->pFormatCtx && player->pFormatCtx->pb && player->pFormatCtx->pb->local_playback)
        send_event(player, PLAYER_EVENTS_LOCAL_PLAYBACK, 1, 0);
    if (player->start_param->is_type_parser) {
        player_cmd_t *msg;
        msg = peek_message(player);
        if (msg && (msg->ctrl_cmd & (CMD_EXIT | CMD_STOP))) {
            goto release0;
        }
    }
    check_di_strong_dm(player);
    log_print("pid[%d]::parse ok , prepare parameters\n", player->player_id);
    if (player->vstream_info.video_index == -1) {
        ret = player_dec_init(player);
        if (ret != PLAYER_SUCCESS) {
            if (check_stop_cmd(player) == 1) {
                set_player_state(player, PLAYER_STOPED);
            } else {
                set_player_state(player, PLAYER_ERROR);
            }
            goto release0;
        }
    }
    if (player->pFormatCtx != NULL && player->pFormatCtx->duration > 0 && player->pFormatCtx->pb != NULL && player->pFormatCtx->pb->opaque != NULL) {
        URLContext *h = (URLContext *)player->pFormatCtx->pb->opaque;
        if (h != NULL && h->prot != NULL && (strcmp(h->prot->name, "shttp") == 0 || strcmp(h->prot->name, "http") == 0)) {
            log_info("[player_thread]set to the network vod\n");
            //player->pFormatCtx->flags|=AVFMT_FLAG_NETWORK_VOD;
        }
    }

    const char * startsync_mode = "/sys/class/tsync/startsync_mode";
    set_sysfs_int(startsync_mode, 2);

    ret = set_media_info(player);
    if (ret != PLAYER_SUCCESS) {
        log_error("pid[%d::player_set_media_info failed!\n", player->player_id);
        set_player_state(player, PLAYER_ERROR);
        goto release0;
    }

    if (am_getconfig_bool_def("media.player.report_enable", 0) == 1) {
        player->media_info.report_para.enable = 1;
        player->media_info.report_para.report_period_ms = am_getconfig_int_def("media.player.report_period", 1000);
    }

    if (am_getconfig_bool_def("media.player.module.avinfo_en", 0) == 1) {
        log_print("enable InfoCollection\n");
        player->media_info.report_para.module_avinfo_enable = 1;

        if (am_getconfig_int_def("ro.kernel.release", 0) == 0x409)
            player->media_info.report_para.module_avinfo_enable = 0;
    }

    int config_ms =0;
    int bufsize = 0;
    int64_t lpbufsize = 0;
    bufsize = am_getconfig_int_def("media.libplayer.startplaybuf",  0);//kb
    config_ms = am_getconfig_int_def("media.libplayer.startplaybuf_ms", 0);//ms
    if (config_ms&&player->pFormatCtx&&(player->pFormatCtx->bit_rate>0)) {
        lpbufsize = ((int64_t)config_ms *player->pFormatCtx->bit_rate)/8000;
        log_print("startplaybuf:%dms,read size:%lld,bitrate:%d,bufsize:%dKb \n", config_ms,lpbufsize,player->pFormatCtx->bit_rate,bufsize);
    }else {
        lpbufsize  = bufsize*1024;
        log_print("startplaybuf:%dms,read size:%lld,bitrate:%d,bufsize:%dKb \n", config_ms,lpbufsize,player->pFormatCtx->bit_rate,bufsize);
    }
    if (lpbufsize && player->pFormatCtx->pb && player->pFormatCtx->pb->is_slowmedia) {
        int64_t pos;
        ffmpeg_buffering_data(player);
        do {
            if (url_interrupt_cb()) {
                break;
            }
            pos=url_buffed_pos(player->pFormatCtx->pb);
            if (pos > 0 && pos < lpbufsize) {
                if (ffmpeg_buffering_data(player) < 0) {
                    player_thread_wait(player, 100 * 1000);
                }
            } else {
                break;
            }
        } while (1);
        log_print("startplaybuf end,pos:%lld\n",pos);
    }

    // Function: buffering av packet to player-> player->av_packet_cache_t
    //
    // prop def:
    // libplayer.cache.apkt_count - minimal count audio to be cached  [default - 2]
    // libplayer.cache.apkt_size  - minimal size audio to be cached   [default - 0]
    // libplayer.cache.vpkt_count - minimal count video to be cacheed [default - 10]
    // libplayer.cache.vpkt_size  - minimal size video to be cached   [default - 0]
    //
    int cache_enable = cache_avpkt_enable(player);
    log_print("amplayer cache_enable=%d\n",cache_enable);
	player->playctrl_info.cache_enable = cache_enable;
    if(cache_enable) {
		if (player->pFormatCtx->pb && player->pFormatCtx->pb->local_playback == 0) {
			player->playctrl_info.cache_buffering = am_getconfig_int_def("libplayer.cache.buffering", 0);
		}
    }
    int64_t init_end = av_gettime();
    int init_cost = init_end - init_start;
    log_print("Init cost_time = %d ms\n", init_cost/1000);
    set_player_state(player, PLAYER_INITOK);
    update_playing_info(player);
    update_player_states(player, 1);
#if 0
    switch (player->pFormatCtx->drm.drm_check_value) {
    case 1: // unauthorized
        set_player_state(player, PLAYER_DIVX_AUTHORERR);
        //send_event(player, PLAYER_EVENTS_STATE_CHANGED,PLAYER_DIVX_AUTHORERR, "Divx Author Failed");
        update_playing_info(player);
        update_player_states(player, 1);
        //  goto release0;
        break;
    case 2: // expired
        //send_event(player, PLAYER_EVENTS_STATE_CHANGED, PLAYER_DIVX_RENTAL_EXPIRED, "Divx Author Expired");
        set_player_state(player, PLAYER_DIVX_RENTAL_EXPIRED);
        update_playing_info(player);
        update_player_states(player, 1);
        //  goto release0;
        break;
    case 3: // rental
        //send_event(player, PLAYER_EVENTS_STATE_CHANGED, PLAYER_DIVX_RENTAL_VIEW, player->pFormatCtx->drm.drm_rental_value);
        set_drm_rental(player, player->pFormatCtx->drm.drm_rental_value);
        set_player_state(player, PLAYER_DIVX_RENTAL_VIEW);
        update_playing_info(player);
        update_player_states(player, 1);
        break;
    default:
        break;
    }
#endif

    no_buffering_start = am_getconfig_bool("media.libplayer.nobufferstart");

    int64_t wait_cmd_start = av_gettime();
    log_print("need_start=%d, no_buffering_start=%d\n", player->start_param->need_start, no_buffering_start);
    if (player->start_param->need_start) {
        int flag = 0;
        do {
            flag = check_start_cmd(player);
            if (flag == 1) {
                break;
            } else if (player->playctrl_info.end_flag == 1 && (!player->playctrl_info.search_flag)) {
                if (player->playctrl_info.request_end_flag) {
                    set_player_state(player, PLAYER_STOPED);
                } else {
                    set_player_state(player, PLAYER_PLAYEND);
                }
                update_playing_info(player);
                update_player_states(player, 1);
                goto release0;
            }
            if (flag == 0) {
                ret = player_offset_init(player);
                if (ret != PLAYER_SUCCESS) {
                    log_error("pid[%d]::prepare offset failed!\n", player->player_id);
                    //set_player_state(player, PLAYER_ERROR);
                    //goto release;
                    if (player->playctrl_info.raw_mode) {
                        url_fseek(player->pFormatCtx->pb, player->data_offset, SEEK_SET);
                    }
                }
            }
            if(no_buffering_start != 1){
                if (ffmpeg_buffering_data(player) < 0) {
                    player_thread_wait(player, 100 * 1000);
                }
            }
            if (flag == -1) {
                usleep(2000);
            }
        } while (1);
    }
#if 0
    /* if drm rental , continue playback here
     * caution:
     * 1. resume play, should not call drmCommitPlayback
     * 2.
     * */
    if (player->pFormatCtx->drm.drm_header && player->pFormatCtx->drm.drm_check_value == 3) {
        ret = drmCommitPlayback();
        if (ret != 0) {
            log_error(" not unauthorized 9:result=%d, err=%d\n", ret, drmGetLastError());
            player->pFormatCtx->drm.drm_check_value = 1; // unauthorized, should popup a dialog
        }
    }
#endif
    int64_t wait_cmd_end = av_gettime();
    int cost = wait_cmd_end - wait_cmd_start;
    log_print("wait START/SEEK cmd cost_time = %d ms\n", cost/1000);

    ret = player_offset_init(player);
    if (ret != PLAYER_SUCCESS) {
        log_error("pid[%d]::prepare offset failed!\n", player->player_id);
        //set_player_state(player, PLAYER_ERROR);
        //goto release;
        if (player->playctrl_info.raw_mode) {
            url_fseek(player->pFormatCtx->pb, player->data_offset, SEEK_SET);
        }
    }

    if (cache_enable == 1) {
        player->start_time = av_gettime();
        avpkt_cache_task_open(player);
        if (player->pFormatCtx->pb != NULL && player->pFormatCtx->pb->local_playback
            && player->vstream_info.has_video
            && player->vstream_info.video_format == VFORMAT_HEVC
            && player->pFormatCtx->hevc_no_csd_data
            && (!player->pFormatCtx->streams[player->vstream_info.video_index]->codec->extradata)) {
            player->playctrl_info.pause_cache = 1;
        } else {
            avpkt_cache_set_cmd(CACHE_CMD_START);
        }
    }

    start_4k_scale(player);

    //case:h265+udp/rtp/rtsp, error frame need enabled.
    if ((av_strstart(player->file_name, "udp://", NULL) ||av_strstart(player->file_name, "rtp://", NULL)
         || av_strstart(player->file_name, "rtsp://", NULL)) && (player->state.full_time <= 0) && am_getconfig_bool_def("media.player.blurdisplay.enable", 1))
    {
        if (player->vstream_info.video_format == VFORMAT_HEVC)
        {
            amsysfs_set_sysfs_int("/sys/module/amvdec_h265/parameters/nal_skip_policy", 0);
            log_print("case:h265+udp/rtsp nal_skip_policy disabled\n");
        }
    }

    log_print("set_subtitle_num:%d\n", player->media_info.stream_info.total_sub_num);
    set_subtitle_num(player->media_info.stream_info.total_sub_num);

    char acCodecStr[8] = {0};
    if (((1 << player->vstream_info.video_format) & PlayerGetVFilterFormat(player, acCodecStr)) != 0) {
        set_player_error_no(player, PLAYER_UNSUPPORT_VCODEC);
        update_player_states(player, 1);
        player->vstream_info.has_video = 0;
    }

    log_print("pid[%d]::decoder prepare\n", player->player_id);
    int64_t start_time = av_gettime();
    ret = player_decoder_init(player);
    if (ret != PLAYER_SUCCESS) {
        log_error("pid[%d]::player_decoder_init failed!\n", player->player_id);
        set_player_state(player, PLAYER_ERROR);
        goto release;
    }

    log_print("after player_decoder_init\n");
    int64_t end_time = av_gettime();
    cost = end_time - start_time;
    log_print("decoder init cost_time :%d ms\n", cost/1000);

    set_cntl_mode(player, TRICKMODE_NONE);
    set_cntl_avthresh(player, AV_SYNC_THRESH);
    set_cntl_syncthresh(player);
    set_sysfs_int("/sys/class/tsync/vpause_flag", 0); // reset vpause flag -> 0
    // 4k HDR not release cma when seek
    if ((player->vstream_info.video_format == VFORMAT_HEVC)&&
            ((player->vstream_info.video_width * player->vstream_info.video_height) > 1920 * 1088)) {
        set_sysfs_int("/sys/module/amvdec_h265/parameters/buffer_mode", 9);
    }

    set_player_state(player, PLAYER_START);
    update_playing_info(player);
    update_player_states(player, 1);

	if (player->playctrl_info.cache_enable == 0) {
		player_mate_init(player, 1000 * 10);
	}

    ffmpeg_seturl_buffered_level(player, 0);
    check_top_botom_invert(player);
    check_use_double_write(player);
    if (player->astream_info.has_audio == 1 &&
        player->vstream_info.has_video == 0 &&
        (player->astream_info.audio_format == AFORMAT_COOK || player->astream_info.audio_format == AFORMAT_SIPR)
       ) {
        AFORMAT_SW_Flag = 1;
    }
    if (player->vstream_info.video_format == VFORMAT_SW || AFORMAT_SW_Flag == 1) {
        log_print("Use SW video decoder\n");

#ifdef SAVE_YUV_FILE
        out_fp = open("./output.yuv", O_CREAT | O_RDWR);
        if (out_fp < 0) {
            log_print("Create output file failed! fd=%d\n", out_fp);
        }
#endif

        av_register_all();

        ic = avcodec_alloc_context();
        if (!ic) {
            log_print("AVCodec Memory error\n");
            ic = NULL;
            goto release;
        }

        if (player->vstream_info.video_format == VFORMAT_SW) {
            ic->codec_id = player->pFormatCtx->streams[player->vstream_info.video_index]->codec->codec_id;
            ic->codec_type = CODEC_TYPE_VIDEO;
            ic->pix_fmt = PIX_FMT_YUV420P;
            picture = avcodec_alloc_frame();
            if (!picture) {
                log_print("Could not allocate picture\n");
                goto release;
            }
        } else if (AFORMAT_SW_Flag == 1) {
            AVCodecContext  *pCodecCtx = player->pFormatCtx->streams[player->astream_info.audio_index]->codec;
            ic->bit_rate       = pCodecCtx->bit_rate;
            ic->sample_rate    = pCodecCtx->sample_rate;
            ic->channels       = pCodecCtx->channels;
            ic->block_align    = pCodecCtx->block_align;
            ic->extradata_size = pCodecCtx->extradata_size;
            ic->codec_id       = pCodecCtx->codec_id;
            ic->codec_type     = CODEC_TYPE_AUDIO;
            if (pCodecCtx->extradata_size > 0) {
                log_print("[%s %d]pCodecCtx->extradata_size/%d\n", __FUNCTION__, __LINE__, pCodecCtx->extradata_size);
                ic->extradata = malloc(pCodecCtx->extradata_size);
                if (ic->extradata != NULL) {
                    memcpy(ic->extradata, pCodecCtx->extradata, pCodecCtx->extradata_size);
                } else {
                    log_print("[%s %d]malloc failed!\n", __FUNCTION__, __LINE__);
                    goto release;
                }
            }
        }
        codec = avcodec_find_decoder(ic->codec_id);
        if (!codec) {
            log_print("Codec not found\n");
            goto release;
        }

        if (avcodec_open(ic, codec) < 0) {
            log_print("Could not open codec\n");
            goto release;
        }
    }
    player->play_start_systemtime_us = player_get_systemtime_ms();
    player->play_last_reset_systemtime_us = player->play_start_systemtime_us;
    log_print("pid[%d]::playback loop... pb=%x\n", player->player_id, player->pFormatCtx->pb);

	if (am_getconfig_int_def("media.libplayer.useblock",0)) 	{
	        if(player->state.full_time > 0){
	             int64_t bitrate = (player->file_size)/(player->state.full_time);
				 char str_tmp[64] = {0};
				 snprintf(str_tmp, sizeof(str_tmp),"%lld", bitrate);
				 property_set("media.libplayer.bitrate",str_tmp);
	             log_print("bitrate:%lld\n", bitrate);
	        }
    }


    if (player->pFormatCtx->pb != NULL && player->pFormatCtx->pb->local_playback &&
       player->vstream_info.has_video && player->vstream_info.video_format == VFORMAT_HEVC &&
                player->pFormatCtx->hevc_no_csd_data &&
                (!player->pFormatCtx->streams[player->vstream_info.video_index]->codec->extradata)) {
        int64_t pos;

        pos = avio_tell(player->pFormatCtx->pb);

        hevc_insert_missed_metainfo(player);
        player->pFormatCtx->hevc_no_pes_header_support = 1;
        player->pFormatCtx->pes_pid = player->vstream_info.video_pid;

        avio_seek(player->pFormatCtx->pb, pos, SEEK_SET);
    }


    if (player->playctrl_info.cache_enable == 1
        && player->playctrl_info.pause_cache == 1) {
        player->playctrl_info.pause_cache = 0;
        avpkt_cache_set_cmd(CACHE_CMD_START);
    }

    if (player->pFormatCtx && player->pFormatCtx->pb
        && player->pFormatCtx->pb->local_playback == 1
        && player->file_type == ASF_FILE
        && player->vstream_info.video_codec_type == VIDEO_DEC_FORMAT_WMV3) {
        char acType[CONFIG_VALUE_MAX] = {0};
        if (am_getconfig("sys.proj.type", acType, NULL) > 0) {
            log_print("sys.proj.type: %s\n", acType);
            if (strcmp(acType, "mobile") == 0) {
                usleep(30000);
                log_print("local play wmv3, sleep 30ms\n");
            }
        }
    }

    // Live Prop Setting
    {
        int live = (player->start_param->is_livemode > 0)?1:0;
        ret = am_setconfig_int("media.amplayer.livemode", live);
        log_print("Live mode, set media.amplayer.livemode to %d. ret:%d\n", live, ret);
    }
    //player loop
    do {
        if ((!(player->vstream_info.video_format == VFORMAT_SW)
             && !(player->vstream_info.video_format == VFORMAT_VC1 && player->vstream_info.video_codec_type == VIDEO_DEC_FORMAT_WMV3)) || \
            (IS_AUIDO_NEED_PREFEED_HEADER(player->astream_info.audio_format) && player->astream_info.has_audio) ||
            (IS_SUB_NEED_PREFEED_HEADER(player->sstream_info.sub_type) && player->sstream_info.has_sub)) {
            if (!player->playctrl_info.trick_wait_flag) {
                pre_header_feeding(player);
            }
        }
        do {
            check_and_modify_livemode(player);
            ret = check_flag(player);
            if (ret == BREAK_FLAG) {
                //log_print("pid[%d]::[player_thread:%d]end=%d valid=%d new=%d pktsize=%d ff %d gettime %lld\n", player->player_id,
                //          __LINE__, player->playctrl_info.end_flag, pkt->avpkt_isvalid, pkt->avpkt_newflag, pkt->data_size, player->playctrl_info.fast_forward, gettime());
                if (!player->playctrl_info.end_flag) {
                    if(player->playctrl_info.hls_forward == 1 || player->playctrl_info.hls_backward == 1){
                        player->playctrl_info.end_flag = 1;
                        if (pkt->avpkt) {
                            av_free_packet(pkt->avpkt);
                            pkt->avpkt_isvalid = 0;
                        }
						break;
                    }
                    if (pkt->avpkt_isvalid) {
                        //player->playctrl_info.read_end_flag = 1;
                        log_print("not go to write packet\n");
                       // goto write_packet;
                        pkt->avpkt_isvalid = 0;

                    } else {
                        player->playctrl_info.end_flag = 1;
                    }
                }
                if (pkt->avpkt) {
                    av_free_packet(pkt->avpkt);
                    pkt->avpkt_isvalid = 0;
                }
                break;
            } else if (ret == CONTINUE_FLAG) {


            }
            if (player->playctrl_info.trick_wait_time > gettime()) {
                continue;
            } else if (player->playctrl_info.trick_wait_flag) {
                break;
            }
			if(player->playctrl_info.hls_forward || player->playctrl_info.hls_backward){
               		check_hls_fffb_endflag(player);
               		if((player->playctrl_info.hls_fffb_endflag)&&
                                player->playctrl_info.read_end_flag == 1 &&
						(player->state.video_bufferlevel < 0.01)){
						if((abs(player->state.current_time - player->state.full_time)< 10)&&
							player->playctrl_info.hls_forward){
                    	log_print("set player force end\n");
						log_print("current_time=%d,full_time=%d\n",
						player->state.current_time,player->state.full_time);
                        player->playctrl_info.end_flag = 1;
                        set_player_state(player, PLAYER_FF_END);
                        send_event(player, PLAYER_EVENTS_FF_END, 0,(unsigned long)(char*)"FF end");
                        update_playing_info(player);
                        update_player_states(player, 1);
                        goto release;

						}
						else if((player->state.current_time < 10)&&
							(player->playctrl_info.hls_backward)){
							player->playctrl_info.last_f_step = 0;
							player->playctrl_info.f_step = 0;
							player->playctrl_info.time_point = 0;
							set_player_state(player, PLAYER_FB_END);
							log_print("not send FB END CMD\n");
							send_event(player,PLAYER_EVENTS_FB_END, 0, (unsigned long)(char*)"FB end");
							update_player_states(player, 1);
                            player->playctrl_info.search_flag = 1;
							player->playctrl_info.end_flag = 0;
							player->astream_info.has_audio =
											player->astream_info.resume_audio;
#if 1
                            if (strcmp(player->pFormatCtx->iformat->name, "mhls")) {
                                ret =  url_setcmd(player->pFormatCtx->pb,
                                        AVCMD_SET_FF_FB_INFO,
                                        0, 0,
                                        (int64_t)player->state.current_ms*1000);
                            }else{
                                ret = av_set_private_parameter(player->pFormatCtx,
                                        AVCMD_SET_FF_FB_INFO,
                                        0, 0,
                                        (int64_t)player->state.current_ms*1000);
                            }
                            log_print("FB ret = %d\n",ret);
                            if(ret == -1){
                                log_print("hls pid[%d]::, can't support fb!\n", player->player_id);
                                set_player_error_no(player, PLAYER_FFFB_UNSUPPORT);
                            }
                            player->playctrl_info.hls_forward = 0;
                            player->playctrl_info.hls_backward = 0;
                            player->playctrl_info.end_flag = 0;
                            log_print("set tickmode none\n");
                            set_vdec_mode(player, TRICKMODE_NONE);
                            amsysfs_set_sysfs_int(MANUAL_DURATION,0);
                            amsysfs_set_sysfs_int(MANUAL_DURATION_CHANGE, 0);
                            set_player_state(player, PLAYER_RUNNING);
                            log_print("FB END. Use normal seek procedure\n");
#endif
                            break;
                        }
                  }
           }

            if (!pkt->avpkt_isvalid) {
                if(am_getconfig_bool("media.amplayer.stepplay") &&
                    amsysfs_get_sysfs_int("/sys/module/amvideo/parameters/amplayer_enable")) {
                    while(1){
                        if (url_interrupt_cb()) {
                            log_print("url interrupt\n");
                            break;
                        }

                        if(amsysfs_get_sysfs_int("/sys/module/amvideo/parameters/amplayer_flag")) {
                            log_print("read_av_packet\n");
                            ret = read_av_packet(player);
                            break;
                        }else{
                            usleep(200*1000);
                            log_print("continue-cotinue-continue\n");
                            continue;
                        }
                    }
                    if(amsysfs_get_sysfs_int("/sys/module/amvideo/parameters/amplayer_flag")) {
                        log_print("amplayer_flag 1 set 0\n");
                        amsysfs_set_sysfs_int("/sys/module/amvideo/parameters/amplayer_flag", 0)
                        ;
                    }
                }else
                ret = read_av_packet(player);
                if (ret != PLAYER_SUCCESS && ret != PLAYER_RD_AGAIN) {
                    if (player->playctrl_info.hls_force_exit != 1 && (ret == PLAYER_RD_FAILED || ret == PLAYER_RD_TIMEOUT)) {
                        ret = (check_to_retry(player) == 0) ? PLAYER_RD_AGAIN : ret;
                    }
                    if (ret != PLAYER_RD_AGAIN || player->playctrl_info.hls_force_exit == 1) { // needn't to retry
                        log_error("pid[%d]::read_av_packet failed!\n", player->player_id);
                        set_player_state(player, PLAYER_ERROR);
                        goto release;
                    }
                    // need to retry
                } else {
                    player->retry_cnt = 0;
                }

				if (player->playctrl_info.cache_enable == 1
					&& player->playctrl_info.pause_cache == 0
					&&(player->playctrl_info.amstream_highlevel == 1
					||(ret == PLAYER_RD_AGAIN && pkt && (pkt->data_size == 0 || pkt->avpkt->size == 0)))) {
					//do nothing
				} else {
	                ret = set_header_info(player);
	                if (ret != PLAYER_SUCCESS) {
	                    log_error("pid[%d]::set_header_info failed! ret=%x\n", player->player_id, -ret);
	                    set_player_state(player, PLAYER_ERROR);
	                    goto release;
	                }
				}
            } else {
                /*low level buf is full ,do buffering or just do wait.*/
                if (player->playctrl_info.cache_enable == 0 && player->enable_rw_on_pause) { /*enabled buffing on paused...*/
                    if (ffmpeg_buffering_data(player) <= 0) {
                        player_thread_wait(player, 100 * 1000); //100ms
                        ///continue;
                    }
                } else {
                    //player_thread_wait(player, 100 * 1000); //100ms
                }
            }
            if ((player->playctrl_info.f_step == 0) &&
                (ret == PLAYER_SUCCESS) &&
                (get_player_state(player) != PLAYER_RUNNING) &&
                (get_player_state(player) != PLAYER_BUFFERING) &&
                (get_player_state(player) != PLAYER_PAUSE)) {
                set_player_state(player, PLAYER_RUNNING);
                update_playing_info(player);
                update_player_states(player, 1);
            }
write_packet:
            if ((player->vstream_info.video_format == VFORMAT_SW) && pkt && (pkt->type == CODEC_VIDEO)) {
                avcodec_decode_video2(ic, picture, &got_picture, pkt->avpkt);
                pkt->data_size = 0;

                if (got_picture) {
#ifdef SAVE_YUV_FILE
                    if (out_fp >= 0) {
                        int i;

                        for (i = 0 ; i < ic->height ; i++) {
                            write(out_fp, picture->data[0] + i * picture->linesize[0], ic->width);
                        }
                        for (i = 0 ; i < ic->height / 2 ; i++) {
                            write(out_fp, picture->data[1] + i * picture->linesize[1], ic->width / 2);
                        }
                        for (i = 0 ; i < ic->height / 2 ; i++) {
                            write(out_fp, picture->data[2] + i * picture->linesize[2], ic->width / 2);
                        }
                    }
#endif
                }
            } else if (AFORMAT_SW_Flag == 1) {
                int bytes_used = 0;
		if (ic) {
                	audio_out_size = MAX(AVCODEC_MAX_AUDIO_FRAME_SIZE, ic->channels * ic->frame_size * sizeof(int16_t));
		}
                audio_out_buf = malloc(audio_out_size);
                if (audio_out_buf != NULL && pkt && ic && pkt->avpkt_isvalid && pkt->avpkt->size > 0 && get_player_state(player) == PLAYER_RUNNING) {
                    memset(audio_out_buf, 0, audio_out_size);
                    bytes_used = ic->codec->decode(ic, audio_out_buf, &audio_out_size, pkt->avpkt);
                    if (audio_out_size > 0) {
                        av_free_packet(pkt->avpkt);
                        pkt->data = audio_out_buf; //it will be free in write_av_packet()
                        pkt->data_size = audio_out_size;
                        write_av_packet(player);
                    }
                }
                if (pkt && pkt->avpkt) {
                    av_free_packet(pkt->avpkt);
                    pkt->avpkt_isvalid = 0;
                }
                if (audio_out_buf != NULL) {
                    free(audio_out_buf);
                    audio_out_buf = NULL;
                    audio_out_size = 0;
                }
		if (pkt) {
                	pkt->data = NULL;
                	pkt->data_size = 0;
		}
            } else {
				if (player->playctrl_info.cache_enable
					&& player->playctrl_info.pause_cache == 0
					&& (player->playctrl_info.amstream_highlevel == 1 || (pkt && pkt->data_size == 0 && pkt->type != CODEC_EOS))) {
					//do nothing
				} else {
					ret = write_av_packet(player);
	                if (ret == PLAYER_WR_FINISH) {
	                    if (player->playctrl_info.f_step == 0) {
	                        log_print("[player_thread]write end!\n");
	                        break;
	                    }
	                } else if (ret != PLAYER_SUCCESS) {
	                    log_print("pid[%d]::write_av_packet failed!\n", player->player_id);
	                    set_player_state(player, PLAYER_ERROR);
	                    goto release;
	                }
				}
            }
            update_playing_info(player);
            update_player_states(player, 0);
            if (check_decoder_worksta(player) != PLAYER_SUCCESS) {
                log_error("pid[%d]::check decoder work status error!\n", player->player_id);
                set_player_state(player, PLAYER_ERROR);
                goto release;
            }
            //check_subtitle_info(player);

            if (pkt && (player->vstream_info.video_format == VFORMAT_SW) && (pkt->type == CODEC_VIDEO)) {
                player->state.current_time = (int)(pkt->avpkt->dts / 1000);
                if (player->state.current_time > player->state.full_time) {
                    player->state.current_time = player->state.full_time;
                }
                if (player->state.current_time < player->state.last_time) {
                    log_print("[%s]curtime<lasttime curtime=%d lastime=%d\n", __FUNCTION__, player->state.current_time, player->state.last_time);
                    player->state.current_time = player->state.last_time;
                }
                player->state.last_time = player->state.current_time;
            }

            if (player->vstream_info.has_video
                && (player->playctrl_info.fast_forward
                    || player->playctrl_info.fast_backward)) {
                if (player->vstream_info.video_format != VFORMAT_SW) {

                    if (last_trick_toggle == -1) {
                        last_trick_toggle = av_gettime();
                        log_print("[%s %d] \n",__FUNCTION__,__LINE__);
                    }
                    // break if display one frame
                    int toggle_count = amsysfs_get_sysfs_int("/sys/module/amvideo/parameters/toggle_count");
                    log_print("FF-FB: get togglecount: %d \n", amsysfs_get_sysfs_int("/sys/module/amvideo/parameters/toggle_count"));

                    int jump_count = am_getconfig_int_def("media.amplayer.ff_count", 1);
                    int manu_dur = am_getconfig_int_def("media.amplayer.ff_manu_dur", 0);
                    int sleep_us = am_getconfig_int_def("media.amplayer.ff_sleepus", 100000);

                    if (player->pFormatCtx->pb != NULL && player->pFormatCtx->pb->local_playback == 1)
                        sleep_us = am_getconfig_int_def("media.amplayer.ff_sleepus_local", 1000000);
                    if(toggle_count >= jump_count) {
                        update_playing_info(player);
                        update_player_states(player, 1);
                        if(manu_dur > 0) {
                            amsysfs_set_sysfs_int(MANUAL_DURATION,96000/manu_dur);
                            amsysfs_set_sysfs_int(MANUAL_DURATION_CHANGE, 1);
                        }
                        sleep_us = ff_step_sleep_convert(player, sleep_us);
                        int64_t now = av_gettime();
                        int64_t diff = (now - last_trick_toggle);
                        if(diff < sleep_us)
                            usleep(sleep_us - diff);
                        last_trick_toggle = av_gettime();

                        if (pkt && pkt->avpkt) {
                            av_free_packet(pkt->avpkt);
                        }
			if (pkt) {
                        pkt->avpkt_isvalid = 0;
			}
                        player->playctrl_info.trick_start_us = -1;
                        break;
                    }

                    if (player->playctrl_info.trick_start_us <= 0) {
                        player->playctrl_info.trick_start_us = gettime();
                    }
                    if(gettime() - player->playctrl_info.trick_start_us >= 2000000) {
                        player->playctrl_info.trick_start_us = -1;
                        last_trick_toggle = av_gettime();
                        log_print("ff wait exceed 2s, jump \n");
                        break;
                    }
                    continue;

                    ret = get_cntl_state(pkt) | player->playctrl_info.seek_offset_same | player->playctrl_info.no_need_more_data;
                    if (ret == 0) {
                        //log_print("more data needed, data size %d\n", pkt->data_size);
                        continue;
                    } else if (ret < 0) {
                        log_error("pid[%d]::get state exception\n", player->player_id);
                        continue;
                    } else {
                        unsigned int curvpts = get_cntl_vpts(pkt);
                        int64_t cur_us = (int64_t)curvpts * 1000 / 90;
                        int wait_time;
                        player->playctrl_info.end_flag = 1;
                        update_playing_info(player);
                        update_player_states(player, 1);

                        int64_t start_time = player->pFormatCtx->start_time;
                        /*
                            If the start_time is longer the 32bit, we use first_time.
                        */
                        if (start_time > 0xffffffff) {
                            if (player->state.first_time > 0) {
                                start_time = (int64_t)player->state.first_time * 1000 / 90;
                            } else {
                                log_print("[%s:%d]get first_time error\n", __FUNCTION__, __LINE__);
                                start_time = start_time & 0xffffffff;
                            }
                        }

                        if ((curvpts == 0) || (curvpts == 1)) {
                            log_print("[%s:%d]curvpts is 0, use timepoint\n", __FUNCTION__, __LINE__);
                            cur_us = player->playctrl_info.time_point * 1000 * 1000 + start_time;
                        }
                        if (0 == player->playctrl_info.trick_start_us) {
                            log_print("[%s:%d]current_ms %d, start_time %lld\n", __FUNCTION__, __LINE__, player->state.current_ms, start_time);
                            player->playctrl_info.trick_start_us = (int64_t)player->state.current_ms * 1000 + start_time;
                            /*
                                If the difference of cur_us and trick_start_us is longer then 20s or negative value, we are sure that the start time is not right.
                                So we assign trick_start_us to cur_us, and the first wait time after ff/fb is 0s.
                            */
                            if ((player->playctrl_info.fast_forward
                                 && (((cur_us - player->playctrl_info.trick_start_us) > 20000000) || ((cur_us - player->playctrl_info.trick_start_us) < 0)))
                                || (player->playctrl_info.fast_backward
                                    && (((player->playctrl_info.trick_start_us - cur_us) > 20000000) || ((player->playctrl_info.trick_start_us - cur_us) < 0)))) {
                                player->playctrl_info.trick_start_us = cur_us;
                            }
                            player->playctrl_info.trick_start_sysus = gettime();
                        }
                        int64_t discontinue_threshold = player->playctrl_info.f_step * 20000000;
                        if ((player->playctrl_info.fast_forward
                             && (((cur_us - player->playctrl_info.trick_start_us) > discontinue_threshold) || ((cur_us - player->playctrl_info.trick_start_us) < 0)))
                            || (player->playctrl_info.fast_backward
                                && (((player->playctrl_info.trick_start_us - cur_us) > discontinue_threshold) || ((player->playctrl_info.trick_start_us - cur_us) < 0)))) {
                            log_print("[%s:%d]reset player->playctrl_info.trick_start_us from %lld to %lld\n", __FUNCTION__, __LINE__, player->playctrl_info.trick_start_us, cur_us);
                            player->playctrl_info.trick_start_us = cur_us;
			    player->playctrl_info.trick_start_sysus = gettime();
                        }
                        if (0 == player->playctrl_info.f_step) {
                            wait_time = 0;
                        } else {
                            if (cur_us > player->playctrl_info.trick_start_us) {
                                wait_time = (cur_us - player->playctrl_info.trick_start_us) / player->playctrl_info.f_step;
                            } else if(cur_us < player->playctrl_info.trick_start_us){
                                wait_time = (player->playctrl_info.trick_start_us - cur_us) / player->playctrl_info.f_step;
                            }else{
				wait_time = 500000;
			    }
                        }
                        //player->playctrl_info.last_trick_us = cur_us;
                        player->playctrl_info.trick_wait_time = wait_time + player->playctrl_info.trick_start_sysus;
                        player->playctrl_info.trick_wait_flag = 1;
                        log_print("[%s:%d]f_step %d, curvpts 0x%x, cur %lld, start %lld, wait %d, trickwait %lld\n",
                                  __FUNCTION__, __LINE__, player->playctrl_info.f_step, curvpts, cur_us, player->playctrl_info.trick_start_us, wait_time, player->playctrl_info.trick_wait_time);
                        //player_thread_wait(player, wait_time);
                        //player_thread_wait(player, (32 >> player->playctrl_info.f_step) * 10 * 1000); //(32 >> player->playctrl_info.f_step)*10 ms
                        break;
                    }
                }
            }

#if 0  // no sync watchdog
            check_avdiff_status(player);
#endif
        } while (!player->playctrl_info.end_flag);

        if ((player->playctrl_info.trick_wait_time > gettime()) && (player->playctrl_info.f_step != 0) &&
			(player->playctrl_info.hls_forward  == 0) && (player->playctrl_info.hls_backward == 0)) {
            continue;
        }

        log_print("wait for play end...(sta:0x%x)\n", get_player_state(player));

        //wait for play end...
        while (!player->playctrl_info.end_flag) {
            ret = check_flag(player);
            if (ret == BREAK_FLAG) {
                if (player->playctrl_info.search_flag
                    || player->playctrl_info.fast_forward
                    || player->playctrl_info.fast_backward) {
                    log_print("[%s:%d] clear read_end_flag\n", __FUNCTION__, __LINE__);
                    player->playctrl_info.read_end_flag = 0;
                }
                break;
            } else if (ret == CONTINUE_FLAG) {
                continue;
            }

            if(player->playctrl_info.fast_forward || player->playctrl_info.fast_backward) {
               break;
            }
			if(player->playctrl_info.hls_forward || player->playctrl_info.hls_backward)
                break;
            if(player->playctrl_info.search_flag == 1){
                log_print("serach_flag 1 break\n");
                player->playctrl_info.read_end_flag = 0;
                break;
            }
            if (player->vstream_info.has_video && player->start_param->local_fd > 0 && check_write_finish(player) == PLAYER_WR_FINISH) {
                if (check_cmcc_jicai_err_file(player->start_param->local_fd)) {
                    codec_para_t * vcodec = get_video_codec(player);
                    if (vcodec) {
                        struct buf_status vbuf;
                        int ret = 0;
                        ret = codec_get_vbuf_state(vcodec, &vbuf);
                        if (ret != 0) {
                            log_error("codec_get_vbuf_state error: %x\n", -ret);
                        } else if (vbuf.data_len == 0) {
                            player->playctrl_info.end_flag = 1;
                            log_print("eos and no date, set end_flag to 1\n");
                        }
                    }
                }
            }

            if (!player->playctrl_info.reset_flag) {
                player_thread_wait(player, 50 * 1000);
            }
            check_decoder_worksta(player);
            if (update_playing_info(player) != PLAYER_SUCCESS) {
                break;
            }

            update_player_states(player, 0);
        }

        log_print("pid[%d]::loop=%d search=%d ff=%d fb=%d reset=%d step=%d switch_ts_program_flag:%d hls_forward:%d hls_backward:%d\n",
                  player->player_id,
                  player->playctrl_info.loop_flag, player->playctrl_info.search_flag,
                  player->playctrl_info.fast_forward, player->playctrl_info.fast_backward,
                  player->playctrl_info.reset_flag, player->playctrl_info.f_step, player->playctrl_info.switch_ts_program_flag,
                  player->playctrl_info.hls_forward, player->playctrl_info.hls_backward);

        exit_flag = (!player->playctrl_info.loop_flag)   &&
                    (!player->playctrl_info.search_flag) &&
                    (!player->playctrl_info.fast_forward) &&
                    (!player->playctrl_info.fast_backward) &&
                    (!player->playctrl_info.reset_flag) &&
                    (!player->playctrl_info.switch_ts_program_flag) &&
                    (!player->playctrl_info.streaming_track_switch_flag) &&
                    (!player->playctrl_info.hls_forward) &&
                    (!player->playctrl_info.hls_backward);

        if (exit_flag) {
            break;
        } else {
            if (get_player_state(player) != PLAYER_SEARCHING
				&& player->playctrl_info.switch_ts_program_flag == 0
				&& player->playctrl_info.streaming_track_switch_flag == 0
				&& player->playctrl_info.f_step != 0) {
                set_auto_refresh_rate(0);
                set_player_state(player, PLAYER_SEARCHING);
                update_playing_info(player);
                update_player_states(player, 1);
            }
            player_switch_para(player);
            if (player->playctrl_info.cache_enable == 1) {
                if (player->playctrl_info.pause_cache == 0) {
                    player->playctrl_info.amstream_highlevel = 0;
                    if (player->playctrl_info.fast_forward == 1
                        || player->playctrl_info.fast_backward == 1) {
                        avpkt_cache_set_cmd(CACHE_CMD_FFFB);//do reset
                        player->playctrl_info.pause_cache = 2;//trickmode_i pause
                    } else if (player->playctrl_info.streaming_track_switch_flag == 1
                        || player->playctrl_info.switch_ts_program_flag == 1) {
                        avpkt_cache_set_cmd(CACHE_CMD_RESET);
                        player->playctrl_info.pause_cache = 1;
                    }
                } else if (player->playctrl_info.pause_cache == 1) {
                    //DO NOTHING
                } else if (player->playctrl_info.pause_cache == 2) {
                    //DO NOTHING
                }
            }

            if (player->playctrl_info.streaming_track_switch_flag == 1) {
                am_packet_t * player_pkt = player->p_pkt;
                player_para_reset(player);
                player_pkt->avpkt_isvalid = 0;
                player_pkt->avpkt_newflag = 0;
                player_pkt->data_size  = 0;
            } else {
                ret = player_reset(player);
                if (ret != PLAYER_SUCCESS) {
                    log_error("pid[%d]::player reset failed(-0x%x)!", player->player_id, -ret);
                    set_player_state(player, PLAYER_ERROR);
                    log_print("reset break\n");
					break;
                }

            }
            /* +[SE][REQ][BUG-168972][jiwei.sun] set trickmode none after reset decoder */
            if (player->playctrl_info.fast_forward == 0
                && player->playctrl_info.fast_backward == 0
                && player->playctrl_info.hls_forward == 0
                && player->playctrl_info.hls_backward == 0) {
                set_cntl_mode(player, TRICKMODE_NONE);
            }

            if (player->playctrl_info.fast_forward || player->playctrl_info.fast_backward) {
                amsysfs_set_sysfs_int("/sys/module/amvideo/parameters/toggle_count", 0);
                log_print("FF-FB: reset togglecount: %d \n", amsysfs_get_sysfs_int("/sys/module/amvideo/parameters/toggle_count"));
            }

			if (player->playctrl_info.hls_forward || player->playctrl_info.hls_backward) {
                if(player->playctrl_info.f_step != 0){
					log_print("set trickmode\n");
					if(player->playctrl_info.hls_forward){
                        if (strcmp(player->pFormatCtx->iformat->name, "mhls")) {
						    ret = url_setcmd(player->pFormatCtx->pb,
											    AVCMD_SET_FF_FB_INFO,
										        1, player->playctrl_info.f_step,
										        (int64_t)player->state.current_ms*1000);
                        } else {

                                log_print("before av_set_private parameter\n");
                                ret = av_set_private_parameter(player->pFormatCtx,
                                                                AVCMD_SET_FF_FB_INFO,
                                                                1, player->playctrl_info.f_step,
                                                                (int64_t)player->state.current_ms*1000);
                        }
                    }else{
                            if (strcmp(player->pFormatCtx->iformat->name, "mhls")) {
						        ret = url_setcmd(player->pFormatCtx->pb,
										 	AVCMD_SET_FF_FB_INFO,
										 2, player->playctrl_info.f_step,
										(int64_t)player->state.current_ms*1000);
                            } else{

                                    log_print("before FB av_set_private parameter\n");
                                    ret = av_set_private_parameter(player->pFormatCtx,
                                                                AVCMD_SET_FF_FB_INFO,
                                                                2, player->playctrl_info.f_step,
                                                                (int64_t)player->state.current_ms*1000);
                            }
                    }
                    log_print("fffb ret = %d, current_us:%lld,step:%d\n", ret, \
								(int64_t)player->state.current_ms*1000, \
								 player->playctrl_info.f_step);

					if(ret == -1){
						log_print("hls pid[%d]::, can't support ff!\n",
									player->player_id);
            			set_player_error_no(player, PLAYER_FFFB_UNSUPPORT);
					}else{
                    	set_vdec_mode(player, TRICKMODE_I);
                        fffb_number = player_frames_in_ff_fb(player->playctrl_info.f_step);
                        log_print("player->playctrl_info.f_step = %d,fffb_number = %d\n",
                                player->playctrl_info.f_step, fffb_number);
                        if(fffb_number != 0)
                            amsysfs_set_sysfs_int(MANUAL_DURATION,96000/fffb_number);
                        amsysfs_set_sysfs_int(MANUAL_DURATION_CHANGE, 1);
                        if (player->playctrl_info.pause_flag) {
                            int has_audio_saved = player->codec->has_audio;
                            if (player->codec->has_audio) {
                                player->codec->has_audio = 0;
                            }
                            log_print("In pause goto ff/fb codec resume\n");
                            codec_resume(player->codec);      //clear pause state
                            player->codec->has_audio = has_audio_saved;
                            player->playctrl_info.pause_flag = 0;
                        }
                        set_player_state(player, PLAYER_RUNNING);
                        set_player_state(player, PLAYER_SEARCHING);
                        if(fffb_number != 0)
                            log_print("After Set trickmode manual duration,manual_duration:%d\n",96000/fffb_number);
					}
				}
                else {
					if (get_player_state(player) == PLAYER_FB_END) {
						player->playctrl_info.f_step = 0;
                        if (strcmp(player->pFormatCtx->iformat->name, "mhls")) {
						    ret =  url_setcmd(player->pFormatCtx->pb,
											    AVCMD_SET_FF_FB_INFO,
											    0, 0,
							    (int64_t)player->state.current_ms*1000);
                        }else{

                            ret = av_set_private_parameter(player->pFormatCtx,
                                                            AVCMD_SET_FF_FB_INFO,
                                                            0, 0,
                                                            (int64_t)player->state.current_ms*1000);
                        }

						log_print("FB ret = %d\n",ret);
						if(ret == -1){
							log_print("hls pid[%d]::, can't support fb!\n", player->player_id);
            				set_player_error_no(player, PLAYER_FFFB_UNSUPPORT);
						}
					}
					player->playctrl_info.hls_forward = 0;
                	player->playctrl_info.hls_backward = 0;
					player->playctrl_info.end_flag = 0;
					log_print("set tickmode none\n");
                    set_vdec_mode(player, TRICKMODE_NONE);
					amsysfs_set_sysfs_int(MANUAL_DURATION,0);
                    amsysfs_set_sysfs_int(MANUAL_DURATION_CHANGE, 0);
                }
            }

            if (player->playctrl_info.end_flag) {
                set_player_state(player, PLAYER_PLAYEND);
				log_print("endflag break\n");
                break;
            }

            if (player->playctrl_info.cache_enable == 1) {
                if (player->playctrl_info.pause_cache == 2) {
                    if (player->playctrl_info.fast_forward == 0
                    && player->playctrl_info.fast_backward == 0) {
                        avpkt_cache_set_cmd(CACHE_CMD_RESET);
                        avpkt_cache_set_cmd(CACHE_CMD_RESET_OK);
                        player->playctrl_info.pause_cache = 0;
                    }
                } else if (player->playctrl_info.pause_cache == 1) {
                    if (player->playctrl_info.search_flag == 1
                        || player->playctrl_info.streaming_track_switch_flag == 1
                        || player->playctrl_info.switch_ts_program_flag == 1
                        ||player->playctrl_info.loop_flag==1
                        ||player->playctrl_info.reset_flag==1) {
                        log_print("player->player_cache_reset_status=%d,seek_async:%d\n", player->player_cache_reset_status, player->seek_async);
                        int count = 0;
                        if (player->seek_async) {
                            while (player->player_cache_reset_status > 0 && count++ < 100) {
                                if (player->player_cache_reset_status == 2) {
                                    break;
                                } else {
                                    usleep(2000);
                                }
                            }
                        }
                        avpkt_cache_set_cmd(CACHE_CMD_RESET_OK);
                        player->playctrl_info.pause_cache = 0;
                        player->player_cache_reset_status = 0;
                        log_print("after search, count=%d\n", count);
                    }
                } else if (player->playctrl_info.pause_cache == 0) {
                    //DO NOTHING
                }
            }

            if (player->playctrl_info.search_flag) {
                set_player_state(player, PLAYER_SEARCHOK);
                update_playing_info(player);
                update_player_states(player, 1);
                // need do status switch otherwise mate may send searchok again
                if (player->playctrl_info.pause_flag) {
                    //fix when search pause status is been set,but action is not done
                    ret = codec_pause(player->codec);
                    if (ret != 0) {
                         log_error("[%s:%d]pause failed!ret=%d\n", __FUNCTION__, __LINE__, ret);
                    }
                    set_player_state(player, PLAYER_PAUSE);
                    update_player_states(player, 1);
                }
                /* +[SE] [BUG][IPTV-393][chuanqi.wang] :reset buffering_check_point when seek*/
                //reset thes contrl var
                player->div_buf_time = (int)am_getconfig_float_def("media.amplayer.divtime", 1);
                if ((player->pFormatCtx->flags & AVFMT_FLAG_NETWORK_VOD)||
                    (player->buffering_enable && player->pFormatCtx->pb && player->pFormatCtx->pb->local_playback == 0&&player->start_param->buffing_force_delay_s > 0)) {
                    player->buffering_force_delay_s = player->mSpeed >= 1 ? 0 : am_getconfig_float_def("media.amplayer.delaybuffering", 10);
                    player->buffering_check_point = 0;
                    log_print("set force delay buffering %f player->mspeed:%f\n", player->buffering_force_delay_s,player->mSpeed);
                }
                if (av_is_segment_media(player->pFormatCtx) || (!strncmp(player->file_name, "rtsp:", strlen("rtsp:")))) {
                    player->buffering_force_delay_s  = am_getconfig_float_def("media.amplayer.delaybuffering", 10);
                    player->buffering_check_point = 0;
                }
                force_buffering_enter(player);
                player->play_last_reset_systemtime_us = player_get_systemtime_ms();
                if (player->playctrl_info.f_step == 0) {
                    // set_black_policy(player->playctrl_info.black_out);
                }
                resume_auto_refresh_rate();
				if(player->playctrl_info.hls_forward || player->playctrl_info.hls_backward){
					log_print("In hls ff/fb,set searching\n");
					set_player_state(player, PLAYER_SEARCHING);
                	update_playing_info(player);
                	update_player_states(player, 1);
				}
            }

            if (player->playctrl_info.reset_flag ||
                (player->playctrl_info.switch_ts_program_flag)) {
                    if (player->playctrl_info.switch_param_flag) {
                        player->playctrl_info.switch_param_flag = 0;
                        if (player->pFormatCtx)
                        player->pFormatCtx->switching_param = 0;
                        log_print("[%s], switch format  end\n", __FUNCTION__);
                    }else if((!player->playctrl_info.fast_forward) && (!player->playctrl_info.fast_backward)){
                        set_black_policy(player->playctrl_info.black_out);
                    }
            }

            player->playctrl_info.search_flag = 0;
            player->playctrl_info.reset_flag = 0;
            player->playctrl_info.end_flag = 0;
            player->playctrl_info.switch_ts_program_flag = 0;
            player->playctrl_info.streaming_track_switch_flag = 0;

            if (player->playctrl_info.cache_enable == 1
                && player->playctrl_info.pause_cache != 0
                && player->playctrl_info.f_step == 0) {
                player->playctrl_info.pause_cache = 0;
            }

            av_packet_release(pkt);
            if (player->start_param->is_livemode == 1 && player->player_need_reset == 1) {
                player_force_enter_buffering(player, 0);
                codec_pause(player->codec);
                //set_player_state(player, PLAYER_BUFFERING);
                update_player_states(player, 1);
            }
        }
    } while (1);
release:
    if (player->vstream_info.video_format == VFORMAT_SW || AFORMAT_SW_Flag == 1) {
#ifdef SAVE_YUV_FILE
        printf("Output file closing\n");
        if (out_fp >= 0) {
            close(out_fp);
        }
        printf("Output file closed\n");
#endif
        if (picture) {
            av_free(picture);
        }
        if (ic) {
            log_print("AVCodec close\n");
            avcodec_close(ic);
            av_free(ic);
        }
    }
    set_cntl_mode(player, TRICKMODE_NONE);
    set_sysfs_int("/sys/class/tsync/vpause_flag", 0);
    set_sysfs_int("/sys/class/video/show_first_frame_nosync", 1);
    amsysfs_set_sysfs_int(MANUAL_DURATION,0);
    set_black_policy(player->playctrl_info.black_out);

release0:
    amsysfs_set_sysfs_int("/sys/module/amvdec_h265/parameters/nal_skip_policy", nal_skip_policy_old);
    set_sysfs_int("/sys/module/amvdec_h265/parameters/buffer_mode", 8);
    resume_auto_refresh_rate();
	if (player->playctrl_info.cache_enable == 1) {
		avpkt_cache_task_close(player);
	} else {
		player_mate_release(player);
	}
    //set_black_policy(0);

    log_print("\npid[%d]player_thread release0 begin...(sta:0x%x)\n", player->player_id, get_player_state(player));

    if (get_player_state(player) == PLAYER_ERROR) {
        if (player->playctrl_info.request_end_flag || check_stop_cmd(player) == 1) {
            /*we have a player end msg,ignore the error*/
            set_player_state(player, PLAYER_STOPED);
            set_player_error_no(player, 0);
        } else {
            int64_t value = 0;
            int rv = ffmpeg_geturl_netstream_info(player, 3, &value);
            if (rv == 0) { //get http download errors for HLS streaming
                log_print("player error,value:%d\n",value);
                ret  = value;
            }
            if (ret == -404 || ret == -500 || ret == -503)
                set_player_error_no(player, -ret);
            else
                set_player_error_no(player, ret);
            log_print("player error, ret = %d\n", ret);
            set_player_error_no(player, ret);
        }

        log_print("player error,force video blackout\n");
        set_black_policy(1);
    }
    update_playing_info(player);
    update_player_states(player, 1);
    av_packet_release(&am_pkt);
    player_para_release(player);
    set_player_state(player, PLAYER_EXIT);
    update_player_states(player, 1);
    dump_file_close();
    stop_4k_scale();
    stop_top_botom_invert();
    stop_double_write(player);
    stop_di_strong_dm();
    if (force_disable_cmcc_report == 1)
        property_set("media.player.cmcc_report.enable", "1");

    if (amsysfs_get_sysfs_int("/sys/module/amvdec_h264/parameters/bad_block_scale")) {
        amsysfs_set_sysfs_int("/sys/module/amvdec_h264/parameters/bad_block_scale", 0);
    }

    if (amsysfs_get_sysfs_int("/sys/module/di/parameters/bypass_all")) {
        amsysfs_set_sysfs_int("/sys/module/di/parameters/bypass_all", 0);
    }

    if (player->error_recovery_mode == 1) {
        if (h264_error_recovery_mode != -1) {
            amsysfs_set_sysfs_int("/sys/module/amvdec_h264/parameters/error_recovery_mode", h264_error_recovery_mode);
        }

        if (mh264_error_proc_policy != -1) {
            amsysfs_set_sysfs_int("/sys/module/amvdec_mh264/parameters/error_proc_policy", mh264_error_proc_policy);
        }
        // mpeg12
        amsysfs_set_sysfs_int("/sys/module/amvdec_mpeg12/parameters/error_frame_skip_level", mpeg12_error_skip_level);
    }

    if (player->cache_release_tid) {
        amthreadpool_pthread_join(player->cache_release_tid, NULL);
    }
    if (player->decode_release_tid) {
        amthreadpool_pthread_join(player->decode_release_tid, NULL);
    }
    if (player->ffmpeg_close_tid) {
        amthreadpool_pthread_join(player->ffmpeg_close_tid, NULL);
    }

    if (player->playctrl_info.black_out == 1
        && amsysfs_get_sysfs_int("/sys/class/video/disable_video") == 1) {
        log_print("update disable_video to 0\n");
        amsysfs_set_sysfs_int("/sys/class/video/disable_video", 0);
    }
    am_setconfig("media.hls.tradtion_trick", "0");
    log_print("\npid[%d]::stop play, exit player thead!(sta:0x%x)\n", player->player_id, get_player_state(player));
    pthread_exit(NULL);

    return NULL;
}
