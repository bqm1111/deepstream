#include "faceApp.h"
#include "DeepStreamAppConfig.h"
#include "message.h"

FaceApp::FaceApp(int argc, char **argv)
{
    m_video_list = std::string(argv[1]);
    gst_init(&argc, &argv);
    m_loop = g_main_loop_new(NULL, FALSE);
    m_pipeline = gst_pipeline_new("face-app");

    m_config = new ConfigManager();
    m_user_callback_data = new user_callback_data();
}

FaceApp::~FaceApp()
{
    delete m_config;
    free_user_callback_data();
}

static gboolean event_thread_func(gpointer arg)
{
    int c = fgetc(stdin);
    switch (c)
    {
    case 'q':
    {
        gst_element_set_state(static_cast<FaceApp *>(arg)->m_pipeline, GST_STATE_NULL);
        g_main_loop_quit(static_cast<FaceApp *>(arg)->getMainloop());
        static_cast<FaceApp *>(arg)->freePipeline();
        break;
    }
    default:
        break;
    }
}

void FaceApp::freePipeline()
{
    g_main_loop_unref(m_loop);
    g_source_remove(m_bus_watch_id);

    nvds_obj_enc_destroy_context(m_face_bin.m_obj_ctx_handle);
    nvds_obj_enc_destroy_context(m_user_callback_data->fullframe_ctx_handle);
}

void FaceApp::init_user_callback_data()
{
    init_curl();
    m_user_callback_data->timestamp = (gchar *)malloc(MAX_TIME_STAMP_LEN);
    m_user_callback_data->session_id = (gchar *)malloc(SESSION_ID_LENGTH);
    uuid_t uuid;
    uuid_generate_random(uuid);
    uuid_unparse_lower(uuid, m_user_callback_data->session_id);
    m_user_callback_data->kafka_producer = new KafkaProducer();
    m_user_callback_data->kafka_producer->init(m_user_callback_data->connection_str);
    m_user_callback_data->fakesink_perf = new SinkPerfStruct;
    m_user_callback_data->video_name = m_video_source_name;

    m_user_callback_data->fullframe_ctx_handle = nvds_obj_enc_create_context();
    if (!m_user_callback_data->fullframe_ctx_handle)
    {
        QDTLog::error("%s:%d Unable to create context\n", __FILE__, __LINE__);
    }
    // Initialize trackers for MOT
    int num_tracker = numVideoSrc();
    m_user_callback_data->trackers = (tracker *)g_malloc0(sizeof(tracker) * num_tracker);
    for (size_t i = 0; i < num_tracker; i++)
        m_user_callback_data->trackers[i] = tracker(
            0.1363697015033318, 91, 0.7510890862625559, 18, 2, 1.);
}

void FaceApp::free_user_callback_data()
{
    free_curl();
    free(m_user_callback_data->session_id);
    free(m_user_callback_data->timestamp);
    delete m_user_callback_data->kafka_producer;
    delete m_user_callback_data->fakesink_perf;
    if (!m_user_callback_data->trackers)
    {
        free(m_user_callback_data->trackers);
    }
    delete m_user_callback_data;
}

void FaceApp::init()
{
    loadConfig();
    init_user_callback_data();
    addVideoSource();
    sequentialDetectAndMOT();

    m_bus = gst_pipeline_get_bus(GST_PIPELINE(m_pipeline));
    m_bus_watch_id = gst_bus_add_watch(m_bus, bus_watch_callback, nullptr);
    g_timeout_add(40, event_thread_func, this);
}

void FaceApp::run()
{
    gst_element_set_state(m_pipeline, GST_STATE_PLAYING);
    g_main_loop_run(m_loop);
}

void FaceApp::loadConfig()
{
    parseJson(m_video_list, m_video_source_name, m_video_source_info);

    m_config->setContext();
    std::shared_ptr<DSAppConfig> appConf = std::dynamic_pointer_cast<DSAppConfig>(m_config->getConfig(ConfigType::DeepStreamApp));

    m_user_callback_data->muxer_output_height = appConf->getProperty(DSAppProperty::STREAMMUX_OUTPUT_HEIGHT).toInt();
    m_user_callback_data->muxer_output_width = appConf->getProperty(DSAppProperty::STREAMMUX_OUTPUT_WIDTH).toInt();
    m_user_callback_data->muxer_batch_size = appConf->getProperty(DSAppProperty::STREAMMUX_BATCH_SIZE).toInt();
    m_user_callback_data->muxer_buffer_pool_size = appConf->getProperty(DSAppProperty::STREAMMUX_BUFFER_POOL_SIZE).toInt();
    m_user_callback_data->muxer_nvbuf_memory_type = appConf->getProperty(DSAppProperty::STREAMMUX_NVBUF_MEMORY_TYPE).toInt();
    m_user_callback_data->muxer_live_source = appConf->getProperty(DSAppProperty::STREAMMUX_LIVE_SOURCE).toBool();
    m_user_callback_data->tiler_cols = appConf->getProperty(DSAppProperty::TILER_COLS).toInt();

    m_user_callback_data->tiler_rows = appConf->getProperty(DSAppProperty::TILER_ROWS).toInt();
    m_user_callback_data->tiler_width = appConf->getProperty(DSAppProperty::TILER_WIDTH).toInt();
    m_user_callback_data->tiler_height = appConf->getProperty(DSAppProperty::TILER_HEIGHT).toInt();
    m_user_callback_data->mot_rawmeta_topic = appConf->getProperty(DSAppProperty::KAFKA_MOT_RAWMETA_TOPIC).toString();
    m_user_callback_data->face_rawmeta_topic = appConf->getProperty(DSAppProperty::KAFKA_FACE_RAWMETA_TOPIC).toString();

    m_user_callback_data->visual_topic = appConf->getProperty(DSAppProperty::KAFKA_VISUAL_TOPIC).toString();
    m_user_callback_data->connection_str = appConf->getProperty(DSAppProperty::KAFKA_CONNECTION_STR).toString();
    m_user_callback_data->curl_address = appConf->getProperty(DSAppProperty::FACE_FEATURE_CURL_ADDRESS).toString();
    m_user_callback_data->face_feature_confidence_threshold = appConf->getProperty(DSAppProperty::FACE_CONFIDENCE_THRESHOLD).toFloat();
    m_user_callback_data->save_crop_img = appConf->getProperty(DSAppProperty::SAVE_CROP_IMG).toBool();
}

static GstPadProbeReturn streammux_src_pad_buffer_probe(GstPad *pad, GstPadProbeInfo *info, gpointer _udata)
{
    user_callback_data *callback_data = reinterpret_cast<user_callback_data *>(_udata);
    generate_ts_rfc3339(callback_data->timestamp, MAX_TIME_STAMP_LEN);
    return GST_PAD_PROBE_OK;
}

void FaceApp::addVideoSource()
{
    int cnt = 0;
    for (const auto &info : m_video_source_info)
    {
        std::string video_path = info[0];
        int source_id = cnt++;
        if (info[2] == std::string("file"))
        {
            m_source.push_back(gst_element_factory_make("filesrc", ("file-source-" + std::to_string(source_id)).c_str()));
            if (fs::path(video_path).extension() == ".avi")
            {
                m_demux.push_back(gst_element_factory_make("tsdemux", ("tsdemux-" + std::to_string(source_id)).c_str()));
            }
            else if (fs::path(video_path).extension() == ".mp4")
            {
                m_demux.push_back(gst_element_factory_make("qtdemux", ("qtdemux-" + std::to_string(source_id)).c_str()));
            }
        }
        else if (info[2] == std::string("rtsp"))
        {
            m_source.push_back(gst_element_factory_make("rtspsrc", ("rtsp-source-" + std::to_string(source_id)).c_str()));
            g_object_set(m_source[source_id], "latency", 300, NULL);
            if (info[1] == "h265")
            {
                m_demux.push_back(gst_element_factory_make("rtph265depay", ("rtph265depay-" + std::to_string(source_id)).c_str()));
            }
            else if (info[1] == "h264")
            {
                m_demux.push_back(gst_element_factory_make("rtph264depay", ("rtph264depay-" + std::to_string(source_id)).c_str()));
            }
            else
            {
                QDTLog::error("Unknown encode type to create video parser\n");
            }
        }
        else
        {
            QDTLog::error("Unknown video input type\n");
        }

        GST_ASSERT(m_source[source_id]);
        GST_ASSERT(m_demux[source_id]);

        if (info[1] == "h265")
        {
            m_parser.push_back(gst_element_factory_make("h265parse", ("h265-parser-" + std::to_string(source_id)).c_str()));
        }
        else if (info[1] == "h264")
        {
            m_parser.push_back(gst_element_factory_make("h264parse", ("h264-parser-" + std::to_string(source_id)).c_str()));
        }
        else
        {
            QDTLog::error("Unknown encode type to create video parser\n");
        }
        GST_ASSERT(m_parser[source_id]);
        m_decoder.push_back(gst_element_factory_make("nvv4l2decoder", ("decoder-" + std::to_string(source_id)).c_str()));
        GST_ASSERT(m_decoder[source_id]);

        g_object_set(m_source[source_id], "location", video_path.c_str(), NULL);

        /* link */
        gst_bin_add_many(
            GST_BIN(m_pipeline), m_source[source_id], m_demux[source_id],
            m_parser[source_id], m_decoder[source_id], NULL);
        if (info[2] == std::string("file"))
        {
            if (!gst_element_link_many(m_source[source_id], m_demux[source_id], NULL))
            {
                gst_printerr("%s:%d could not link elements in camera source\n", __FILE__, __LINE__);
                throw std::runtime_error("");
            }
            // link tsdemux to h265parser
            g_signal_connect(m_demux[source_id], "pad-added", G_CALLBACK(addnewPad),
                             m_parser[source_id]);
        }
        else if (info[2] == std::string("rtsp"))
        {
            g_signal_connect(m_source[source_id], "pad-added", G_CALLBACK(addnewPad),
                             m_demux[source_id]);
            if (!gst_element_link_many(m_demux[source_id], m_parser[source_id], NULL))
            {
                gst_printerr("%s:%d could not link elements in camera source\n", __FILE__, __LINE__);
                throw std::runtime_error("");
            }
        }

        if (!gst_element_link_many(m_parser[source_id], m_decoder[source_id], NULL))
        {
            gst_printerr("%s:%d could not link elements in camera source\n", __FILE__, __LINE__);
            throw std::runtime_error("");
        }
    }

    // Add streammuxer
    m_stream_muxer = gst_element_factory_make("nvstreammux", "streammuxer");
    GST_ASSERT(m_stream_muxer);
    g_object_set(m_stream_muxer, "width", m_user_callback_data->muxer_output_width,
                 "height", m_user_callback_data->muxer_output_height,
                 "batch-size", m_user_callback_data->muxer_batch_size,
                 "buffer-pool-size", m_user_callback_data->muxer_buffer_pool_size,
                 "nvbuf-memory-type", m_user_callback_data->muxer_nvbuf_memory_type,
                 "batched-push-timeout", 220000,
                 "live-source", TRUE,
                 NULL);
    GstPad *streammux_pad = gst_element_get_static_pad(m_stream_muxer, "src");
    GST_ASSERT(streammux_pad);
    gst_pad_add_probe(streammux_pad, GST_PAD_PROBE_TYPE_BUFFER, streammux_src_pad_buffer_probe,
                      m_user_callback_data, NULL);
    g_object_unref(streammux_pad);

    gst_bin_add(GST_BIN(m_pipeline), m_stream_muxer);

    for (int i = 0; i < numVideoSrc(); i++)
    {
        GstPad *decoder_srcpad = gst_element_get_static_pad(m_decoder[i], "src");
        GST_ASSERT(decoder_srcpad);

        GstPad *muxer_sinkpad = gst_element_get_request_pad(m_stream_muxer, ("sink_" + std::to_string(i)).c_str());
        GST_ASSERT(muxer_sinkpad);

        GstPadLinkReturn pad_link_return = gst_pad_link(decoder_srcpad, muxer_sinkpad);
        if (GST_PAD_LINK_FAILED(pad_link_return))
        {
            gst_printerr("%s:%d could not link decoder and muxer, reason %d\n", __FILE__, __LINE__, pad_link_return);
            throw std::runtime_error("");
        }
        gst_object_unref(decoder_srcpad);
        gst_object_unref(muxer_sinkpad);
    }
}

void FaceApp::init_curl()
{
    m_user_callback_data->curl = curl_easy_init();
    CURL *m_curl = m_user_callback_data->curl;
    assert(m_curl);

    /* copy from postman */
    curl_easy_setopt(m_curl, CURLOPT_CUSTOMREQUEST, "POST");
    curl_easy_setopt(m_curl, CURLOPT_URL, m_user_callback_data->curl_address.c_str());

    // curl_easy_setopt(m_curl, CURLOPT_VERBOSE, 11);

    /* HTTP/2 */
    curl_easy_setopt(m_curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2_0);

    /* No SSL */
    curl_easy_setopt(m_curl, CURLOPT_SSL_VERIFYPEER, 0);

    /* wait for pipe connection to confirm*/
    curl_easy_setopt(m_curl, CURLOPT_PIPEWAIT, 1L);

    curl_easy_setopt(m_curl, CURLOPT_TIMEOUT_MS, 200);

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    curl_easy_setopt(m_curl, CURLOPT_HTTPHEADER, headers);
}

void FaceApp::free_curl()
{
    curl_easy_cleanup(m_user_callback_data->curl);
}

GstElement *FaceApp::getPipeline()
{
    return m_pipeline;
}

int FaceApp::numVideoSrc()
{
    return m_video_source_name.size();
}

static GstPadProbeReturn encode_and_send(GstPad *pad, GstPadProbeInfo *info, gpointer _udata)
{
    GstBuffer *buf = reinterpret_cast<GstBuffer *>(info->data);
    GST_ASSERT(buf);
    if (!buf)
    {
        return GST_PAD_PROBE_OK;
    }
    GstMapInfo inmap = GST_MAP_INFO_INIT;
    if (!gst_buffer_map(buf, &inmap, GST_MAP_READ))
    {
        QDTLog::error("Error: Failed to map gst buffer\n");
        gst_buffer_unmap(buf, &inmap);
    }
    NvBufSurface *ip_surf = (NvBufSurface *)inmap.data;
    gst_buffer_unmap(buf, &inmap);

    user_callback_data *callback_data = reinterpret_cast<user_callback_data *>(_udata);

    NvDsBatchMeta *batch_meta = gst_buffer_get_nvds_batch_meta(buf);

    for (NvDsMetaList *l_frame = batch_meta->frame_meta_list; l_frame != NULL; l_frame = l_frame->next)
    {
        NvDsFrameMeta *frame_meta = (NvDsFrameMeta *)(l_frame->data);

        // attack a mfake object whose class id is MFAKE_CLASS_ID
        NvDsObjectMeta *mfake_meta = nvds_acquire_obj_meta_from_pool(batch_meta);
        mfake_meta->unique_component_id = 1;
        mfake_meta->confidence = 1.0;
        mfake_meta->class_id = MFAKE_CLASS_ID;
        mfake_meta->object_id = UNTRACKED_OBJECT_ID;

        mfake_meta->detector_bbox_info.org_bbox_coords.left = 0.0f;
        mfake_meta->detector_bbox_info.org_bbox_coords.top = 0.0f;
        mfake_meta->detector_bbox_info.org_bbox_coords.width = 1000;
        mfake_meta->detector_bbox_info.org_bbox_coords.height = 1000;
        // mfake_meta->detector_bbox_info.org_bbox_coords.height = frame_meta->source_frame_height;
        // mfake_meta->detector_bbox_info.org_bbox_coords.height = frame_meta->source_frame_height;

        mfake_meta->rect_params.top = 0.0f;
        mfake_meta->rect_params.left = 0.0f;
        mfake_meta->rect_params.width = float(frame_meta->source_frame_width);
        mfake_meta->rect_params.height = float(frame_meta->source_frame_height);

        // QDTLog::debug("frame_meta width {} height {}", frame_meta->source_frame_width, frame_meta->source_frame_height);

        nvds_add_obj_meta_to_frame(frame_meta, mfake_meta, NULL);

        NvDsObjEncUsrArgs userData = {0};
        userData.saveImg = FALSE;
        userData.attachUsrMeta = TRUE;

        userData.scaleImg = TRUE;
        userData.scaledWidth = callback_data->fullframe_encode_scale_width;
        userData.scaledHeight = callback_data->fullframe_encode_scale_height;

        userData.quality = 50;

        nvds_obj_enc_process((NvDsObjEncCtxHandle)callback_data->fullframe_ctx_handle, &userData, ip_surf, mfake_meta, frame_meta);
    }

    nvds_obj_enc_finish((NvDsObjEncCtxHandle)callback_data->fullframe_ctx_handle);

    for (NvDsMetaList *l_frame = batch_meta->frame_meta_list; l_frame != NULL; l_frame = l_frame->next)
    {
        NvDsFrameMeta *frame_meta = (NvDsFrameMeta *)(l_frame->data);
        for (NvDsMetaList *l_obj = frame_meta->obj_meta_list; l_obj != NULL; l_obj = l_obj->next)
        {
            NvDsObjectMeta *obj_meta = reinterpret_cast<NvDsObjectMeta *>(l_obj->data);
            if (obj_meta->class_id != MFAKE_CLASS_ID)
            {
                continue;
            }
            for (NvDsMetaList *l_user = obj_meta->obj_user_meta_list; l_user != NULL; l_user = l_user->next)
            {
                NvDsUserMeta *user_meta = reinterpret_cast<NvDsUserMeta *>(l_user->data);
                // get encoded image from mfake_meta
                if (user_meta->base_meta.meta_type != NVDS_CROP_IMAGE_META)
                {
                    continue;
                }

                NvDsObjEncOutParams *enc_jpeg_image = (NvDsObjEncOutParams *)user_meta->user_meta_data;
                // face_msg_sub_meta->encoded_img = g_strdup(b64encode(enc_jpeg_image->outBuffer, enc_jpeg_image->outLen));

                XFaceVisualMsg *msg_meta_content = (XFaceVisualMsg *)g_malloc0(sizeof(XFaceVisualMsg));
                msg_meta_content->timestamp = g_strdup(callback_data->timestamp);
                msg_meta_content->cameraId = g_strdup(std::string(callback_data->video_name[frame_meta->source_id]).c_str());
                msg_meta_content->frameId = frame_meta->frame_num;
                msg_meta_content->sessionId = g_strdup(callback_data->session_id);
                msg_meta_content->full_img = g_strdup(b64encode(enc_jpeg_image->outBuffer, enc_jpeg_image->outLen));

                msg_meta_content->width = callback_data->fullframe_encode_scale_width;
                msg_meta_content->height = callback_data->fullframe_encode_scale_height;
                msg_meta_content->num_channel = 3 /*bgr_frame.channels()*/;

                gchar *message = generate_XFace_visual_message(msg_meta_content);
                RdKafka::ErrorCode err = callback_data->kafka_producer->producer->produce(callback_data->visual_topic,
                                                                                          RdKafka::Topic::PARTITION_UA,
                                                                                          RdKafka::Producer::RK_MSG_FREE,
                                                                                          (gchar *)message,
                                                                                          std::string(message).length(),
                                                                                          NULL, 0,
                                                                                          0, NULL, NULL);

                freeXFaceVisualMsg(msg_meta_content);
                callback_data->kafka_producer->counter++;
                if (err != RdKafka::ERR_NO_ERROR)
                {
                    if (err == RdKafka::ERR__QUEUE_FULL)
                    {
                        /* If the internal queue is full, wait for
                         * messages to be delivered and then retry.
                         * The internal queue represents both
                         * messages to be sent and messages that have
                         * been sent or failed, awaiting their
                         * delivery report callback to be called.
                         *
                         * The internal queue is limited by the
                         * configuration property
                         * queue.buffering.max.messages */
                        if (callback_data->kafka_producer->counter > 10)
                        {
                            callback_data->kafka_producer->counter = 0;
                            callback_data->kafka_producer->producer->poll(100);
                        }
                    }
                }
            }
        } // obj_meta_list
    }     // frame_meta_list
    return GST_PAD_PROBE_OK;
}

GstPadProbeReturn fakesink_pad_buffer_probe(GstPad *pad, GstPadProbeInfo *info, gpointer _udata)
{
    GstBuffer *buf = reinterpret_cast<GstBuffer *>(info->data);
    GST_ASSERT(buf);
    if (!buf)
    {
        return GST_PAD_PROBE_OK;
    }

    if (_udata != nullptr)
    {
        /* do speed mesurement */
        SinkPerfStruct *sink_perf_struct = reinterpret_cast<SinkPerfStruct *>(_udata);
        sink_perf_struct->check_start();
        sink_perf_struct->update();
        sink_perf_struct->log();

        if (nvds_enable_latency_measurement)
        {
            // NvDsFrameLatencyInfo latency_info[1];
            NvDsFrameLatencyInfo *latency_info = (NvDsFrameLatencyInfo *)malloc(20 * sizeof(NvDsFrameLatencyInfo));
            int num_sources_in_batch = nvds_measure_buffer_latency(buf, latency_info);
            for (int i = 0; i < num_sources_in_batch; i++)
            {
                QDTLOG_DEBUG("source_id={} frame_num={} frame latancy={}",
                             latency_info[i].source_id,
                             latency_info[i].frame_num,
                             latency_info[i].latency);
            }
            free(latency_info);
        }
    }

    return GST_PAD_PROBE_OK;
}

void FaceApp::sequentialDetectAndMOT()
{
    // ======================== MOT BRANCH ========================
    std::shared_ptr<NvInferMOTBinConfig> mot_configs = std::make_shared<NvInferMOTBinConfig>(MOT_PGIE_CONFIG_PATH, MOT_SGIE_CONFIG_PATH);
    m_mot_bin.setConfig(mot_configs);
    // remember to acquire trackerList before createBin
    m_mot_bin.acquireUserData(m_user_callback_data);
    m_mot_bin.createInferBin();
    m_mot_bin.getMasterBin(m_mot_elem);

    // ======================== DETECT BRANCH ========================
    std::shared_ptr<NvInferFaceBinConfig> face_configs = std::make_shared<NvInferFaceBinConfig>(FACEID_PGIE_CONFIG_PATH, FACEID_SGIE_CONFIG_PATH, FACEID_ALIGN_CONFIG_PATH);

    m_face_bin.setConfig(face_configs);
    // remember to acquire curl before createBin
    m_face_bin.acquireUserData(m_user_callback_data);
    m_face_bin.createInferBin();
    m_face_bin.getMasterBin(m_face_elem);

    // ========================================================================
    NvInferBinBase bin;
    bin.acquireUserData(m_user_callback_data);
    m_tiler = bin.createNonInferPipeline(m_pipeline);

    m_video_convert = gst_element_factory_make("nvvideoconvert", "video-converter");
    g_object_set(G_OBJECT(m_video_convert), "nvbuf-memory-type", 3, NULL);
    m_capsfilter = gst_element_factory_make("capsfilter", std::string("sink-capsfilter-rgba").c_str());
    GST_ASSERT(m_capsfilter);
    GstCaps *caps = gst_caps_from_string("video/x-raw(memory:NVMM), format=(string)RGBA");
    GST_ASSERT(caps);
    g_object_set(G_OBJECT(m_capsfilter), "caps", caps, NULL);
    //
    m_tee = gst_element_factory_make("tee", "tee-split");
    m_queue_infer = gst_element_factory_make("queue", "queue-infer");
    m_queue_encode = gst_element_factory_make("queue", "queue-encode");

    m_fakesink = gst_element_factory_make("fakesink", "osd");
    gst_bin_add_many(GST_BIN(m_pipeline), m_tee, m_queue_infer, m_queue_encode, m_video_convert, m_capsfilter, m_fakesink, NULL);
    gst_bin_add_many(GST_BIN(m_pipeline), m_mot_elem, m_face_elem, NULL);

    if (!gst_element_link_many(m_stream_muxer, m_tee, NULL))
    {
        QDTLog::error("Cannot link mot and face bin {}:{}", __FILE__, __LINE__);
    }

    if (!gst_element_link_many(m_queue_encode, m_video_convert, m_capsfilter, m_fakesink, NULL))
    {
        QDTLog::error("Cannot link mot and face bin {}:{}", __FILE__, __LINE__);
    }

    if (!gst_element_link_many(m_queue_infer, m_mot_elem, m_face_elem, bin.m_tiler, NULL))
    {
        QDTLog::error("Cannot link mot and face bin {}:{}", __FILE__, __LINE__);
    }

    // Link queue infer
    GstPad *sink_pad = gst_element_get_static_pad(m_queue_infer, "sink");
    GstPad *queue_infer_pad = gst_element_get_request_pad(m_tee, "src_%u");
    if (!queue_infer_pad)
    {
        g_printerr("Unable to get request pads\n");
    }

    if (gst_pad_link(queue_infer_pad, sink_pad) != GST_PAD_LINK_OK)
    {
        g_printerr("Unable to link tee and message converter\n");
        gst_object_unref(sink_pad);
    }
    gst_object_unref(sink_pad);

    // Link queue encode
    sink_pad = gst_element_get_static_pad(m_queue_encode, "sink");
    GstPad *queue_encode_pad = gst_element_get_request_pad(m_tee, "src_%u");
    if (!queue_encode_pad)
    {
        g_printerr("Unable to get request pads\n");
    }

    if (gst_pad_link(queue_encode_pad, sink_pad) != GST_PAD_LINK_OK)
    {
        g_printerr("Unable to link tee and message converter\n");
        gst_object_unref(sink_pad);
    }
    gst_object_unref(sink_pad);
    gst_object_unref(queue_encode_pad);
    gst_object_unref(queue_infer_pad);
    GstPad *capsfilter_src_pad = gst_element_get_static_pad(m_capsfilter, "src");
    GST_ASSERT(capsfilter_src_pad);

    gst_pad_add_probe(capsfilter_src_pad, GST_PAD_PROBE_TYPE_BUFFER, encode_and_send,
                      m_user_callback_data, NULL);

    g_object_unref(capsfilter_src_pad);

    GstPad *fakesink_pad = gst_element_get_static_pad(m_fakesink, "sink");
    GST_ASSERT(fakesink_pad);
    gst_pad_add_probe(fakesink_pad, GST_PAD_PROBE_TYPE_BUFFER, fakesink_pad_buffer_probe,
                      m_user_callback_data->fakesink_perf, NULL);
    g_object_unref(fakesink_pad);

    GST_DEBUG_BIN_TO_DOT_FILE(GST_BIN(m_pipeline), GST_DEBUG_GRAPH_SHOW_ALL, "test_run");
}
