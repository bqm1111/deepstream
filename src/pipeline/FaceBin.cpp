#include "FaceBin.h"
#include "QDTLog.h"

void NvInferFaceBin::createInferBin()
{
    m_masterBin = gst_bin_new("face-bin");
    GST_ASSERT(m_masterBin);

    m_pgie = gst_element_factory_make("nvinfer", "face-nvinfer");
    GST_ASSERT(m_pgie);
    GstRegistry *registry;
    registry = gst_registry_get();
    gst_registry_scan_path(registry, "src/facealignment");

    GstPad *pgie_src_pad = gst_element_get_static_pad(m_pgie, "src");
    GST_ASSERT(pgie_src_pad);
    m_obj_ctx_handle = nvds_obj_enc_create_context();
    if (!m_obj_ctx_handle)
    {
        QDTLog::error("%s:%d Unable to create context\n", __FILE__, __LINE__);
    }
    gst_pad_add_probe(pgie_src_pad, GST_PAD_PROBE_TYPE_BUFFER, this->pgie_src_pad_buffer_probe,
                      (gpointer)m_obj_ctx_handle, NULL);
    aligner = gst_element_factory_make("nvfacealign", "faceid-aligner");
    GST_ASSERT(aligner);

    m_sgie = gst_element_factory_make("nvinfer", "faceid-secondary-inference");
    GST_ASSERT(m_sgie);

    GstPad *sgie_src_pad = gst_element_get_static_pad(m_sgie, "src");
    GST_ASSERT(sgie_src_pad);
    // gst_pad_add_probe(sgie_src_pad, GST_PAD_PROBE_TYPE_BUFFER, sgie_face_src_pad_buffer_probe, nullptr, NULL);
    // Properties
    g_object_set(m_pgie, "config-file-path", m_configs->pgie_config_path.c_str(), NULL);
    g_object_set(m_pgie, "output-tensor-meta", TRUE, NULL);
    g_object_set(aligner, "config-file-path", std::dynamic_pointer_cast<NvInferFaceBinConfig>(m_configs)->aligner_config_path.c_str(), NULL);

    g_object_set(m_sgie, "config-file-path", m_configs->sgie_config_path.c_str(), NULL);
    g_object_set(m_sgie, "input-tensor-meta", TRUE, NULL);
    g_object_set(m_sgie, "output-tensor-meta", TRUE, NULL);

    face_user_data *callback_data = new face_user_data;
    callback_data->curl = m_curl;
    callback_data->video_source_name = m_video_source_name;
    gst_nvinfer_raw_output_generated_callback out_callback = this->sgie_output_callback;
    g_object_set(m_sgie, "raw-output-generated-callback", out_callback, NULL);
    g_object_set(m_sgie, "raw-output-generated-userdata", reinterpret_cast<void *>(callback_data), NULL);

    gst_bin_add_many(GST_BIN(m_masterBin), m_pgie, aligner, m_sgie, NULL);
    gst_element_link_many(m_pgie, aligner, m_sgie, NULL);

    // Add ghost pads
    GstPad *pgie_sink_pad = gst_element_get_static_pad(m_pgie, "sink");
    GST_ASSERT(pgie_sink_pad);

    GstPad *sink_ghost_pad = gst_ghost_pad_new("sink", pgie_sink_pad);
    GST_ASSERT(sink_ghost_pad);

    // GstPad *src_ghost_pad = gst_ghost_pad_new("src", pgie_src_pad);
    GstPad *src_ghost_pad = gst_ghost_pad_new("src", sgie_src_pad);

    GST_ASSERT(src_ghost_pad);

    gst_pad_set_active(sink_ghost_pad, true);
    gst_pad_set_active(src_ghost_pad, true);

    gst_element_add_pad(m_masterBin, sink_ghost_pad);
    gst_element_add_pad(m_masterBin, src_ghost_pad);

    gst_object_unref(pgie_src_pad);
    gst_object_unref(pgie_sink_pad);
    gst_object_unref(sgie_src_pad);
}

void NvInferFaceBin::createDetectBin()
{
    m_masterBin = gst_bin_new("face-bin");
    GST_ASSERT(m_masterBin);

    m_pgie = gst_element_factory_make("nvinfer", "face-nvinfer");
    GST_ASSERT(m_pgie);

    GstPad *pgie_src_pad = gst_element_get_static_pad(m_pgie, "src");
    GST_ASSERT(pgie_src_pad);
    gst_pad_add_probe(pgie_src_pad, GST_PAD_PROBE_TYPE_BUFFER, this->pgie_src_pad_buffer_probe, nullptr, NULL);

    // Properties
    g_object_set(m_pgie, "config-file-path", m_configs->pgie_config_path.c_str(), NULL);
    g_object_set(m_pgie, "output-tensor-meta", TRUE, NULL);

    gst_bin_add_many(GST_BIN(m_masterBin), m_pgie, NULL);
    
    // Add ghost pads
    GstPad *pgie_sink_pad = gst_element_get_static_pad(m_pgie, "sink");
    GST_ASSERT(pgie_sink_pad);

    GstPad *sink_ghost_pad = gst_ghost_pad_new("sink", pgie_sink_pad);
    GST_ASSERT(sink_ghost_pad);

    GstPad *src_ghost_pad = gst_ghost_pad_new("src", pgie_src_pad);
    GST_ASSERT(src_ghost_pad);

    gst_pad_set_active(sink_ghost_pad, true);
    gst_pad_set_active(src_ghost_pad, true);

    gst_element_add_pad(m_masterBin, sink_ghost_pad);
    gst_element_add_pad(m_masterBin, src_ghost_pad);
    //
    gst_object_unref(pgie_src_pad);
}

void NvInferFaceBin::acquireFaceUserData(CURL *curl, std::vector<std::string> video_source_list)
{
    m_curl = curl;
    m_video_source_name = video_source_list;
}

void NvInferFaceBin::setMsgBrokerConfig()
{
    g_object_set(G_OBJECT(m_metadata_msgconv), "config", MSG_CONFIG_PATH, NULL);
    g_object_set(G_OBJECT(m_metadata_msgconv), "msg2p-lib", KAFKA_MSG2P_LIB, NULL);
    g_object_set(G_OBJECT(m_metadata_msgconv), "payload-type", NVDS_PAYLOAD_CUSTOM, NULL);
    g_object_set(G_OBJECT(m_metadata_msgconv), "msg2p-newapi", 0, NULL);
    g_object_set(G_OBJECT(m_metadata_msgconv), "frame-interval", 30, NULL);

    g_object_set(G_OBJECT(m_metadata_msgbroker), "proto-lib", KAFKA_PROTO_LIB,
                 "conn-str", m_params.connection_str.c_str(), "sync", FALSE, NULL);

    g_object_set(G_OBJECT(m_metadata_msgbroker), "topic", m_params.metadata_topic.c_str(), NULL);
}

void NvInferFaceBin::attachProbe()
{
    GstPad *tiler_sink_pad = gst_element_get_static_pad(m_tiler, "sink");
    GST_ASSERT(tiler_sink_pad);
    gst_pad_add_probe(tiler_sink_pad, GST_PAD_PROBE_TYPE_BUFFER, tiler_sink_pad_buffer_probe,
                      reinterpret_cast<gpointer>(m_tiler), NULL);
    g_object_unref(tiler_sink_pad);

    GstPad *osd_sink_pad = gst_element_get_static_pad(m_osd, "sink");
    GST_ASSERT(osd_sink_pad);
    gst_pad_add_probe(osd_sink_pad, GST_PAD_PROBE_TYPE_BUFFER, osd_sink_pad_buffer_probe,
                      reinterpret_cast<gpointer>(m_tiler), NULL);
    gst_object_unref(osd_sink_pad);
}
