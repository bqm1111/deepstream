#include "PipelineHandler.h"

AppPipeline::AppPipeline(std::string pipeline_name, GstAppParam params)
{
    m_pipeline_name = pipeline_name;
    m_gstparams = params;
    m_pipeline = gst_pipeline_new(m_pipeline_name.c_str());
}

AppPipeline::~AppPipeline()
{
    gst_object_unref(GST_OBJECT(m_pipeline));
    gst_element_release_request_pad(m_tee, m_tee_msg_pad);
    gst_element_release_request_pad(m_tee, m_tee_display_pad);
    gst_object_unref(m_tee_msg_pad);
    gst_object_unref(m_tee_display_pad);
}

void AppPipeline::create(std::string pipeline_name, GstAppParam params)
{
    m_pipeline_name = pipeline_name;
    m_gstparams = params;
    m_pipeline = gst_pipeline_new(m_pipeline_name.c_str());
}

int AppPipeline::numVideoSrc()
{
    return m_video_source.size();
}

GstElement *AppPipeline::add_video_source(std::string video_path, std::string video_name)
{
    m_video_source[video_name] = numVideoSrc() + 1;
    int source_id = numVideoSrc() - 1;

    m_source.push_back(gst_element_factory_make("filesrc", ("file-source-" + std::to_string(source_id)).c_str()));
    GST_ASSERT(m_source[source_id]);

    if (fs::path(video_path).extension() == ".avi")
    {
        m_demux.push_back(gst_element_factory_make("tsdemux", ("tsdemux-" + std::to_string(source_id)).c_str()));
    }
    else if (fs::path(video_path).extension() == ".mp4")
    {
        m_demux.push_back(gst_element_factory_make("qtdemux", ("qtdemux-" + std::to_string(source_id)).c_str()));
    }

    m_parser.push_back(gst_element_factory_make("h265parse", ("h265-parser-" + std::to_string(source_id)).c_str()));
    GST_ASSERT(m_parser[source_id]);
    m_decoder.push_back(gst_element_factory_make("nvv4l2decoder", ("decoder-" + std::to_string(source_id)).c_str()));
    GST_ASSERT(m_decoder[source_id]);

    std::cout << "Input video path: " << video_path << std::endl;
    g_object_set(m_source[source_id], "location", video_path.c_str(), NULL);

    /* link */
    gst_bin_add_many(
        GST_BIN(m_pipeline), m_source[source_id], m_demux[source_id],
        m_parser[source_id], m_decoder[source_id], NULL);

    if (!gst_element_link_many(m_source[source_id], m_demux[source_id], NULL))
    {
        gst_printerr("%s:%d could not link elements in camera source\n", __FILE__, __LINE__);
        throw std::runtime_error("");
    }
    if (!gst_element_link_many(m_parser[source_id], m_decoder[source_id], NULL))
    {
        gst_printerr("%s:%d could not link elements in camera source\n", __FILE__, __LINE__);
        throw std::runtime_error("");
    }
    // link tsdemux to h265parser
    g_signal_connect(m_demux[source_id], "pad-added", G_CALLBACK(addnewPad),
                     m_parser[source_id]);

    return m_decoder[source_id];
}

void AppPipeline::linkMuxer()
{
    m_muxer = gst_element_factory_make("nvstreammux", "streammuxer");
    g_object_set(m_muxer, "width", m_gstparams.muxer_output_width,
                 "height", m_gstparams.muxer_output_height,
                 "batch-size", numVideoSrc(),
                 "batched-push-timeout", 220000, // 5FPS
                 NULL);
    gst_bin_add(GST_BIN(m_pipeline), m_muxer);

    for (int i = 0; i < numVideoSrc(); i++)
    {
        GstPad *decoder_srcpad = gst_element_get_static_pad(m_decoder[i], "src");
        GST_ASSERT(decoder_srcpad);

        GstPad *muxer_sinkpad = gst_element_get_request_pad(m_muxer, ("sink_" + std::to_string(i)).c_str());
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

void AppPipeline::link(GstElement *in_elem, GstElement *out_elem)
{
    if (!gst_element_link_many(in_elem, out_elem, NULL))
    {
        gst_printerr("Could not link elements: %s%d\n", __FILE__, __LINE__);
    }
}

GstElement *AppPipeline::createGeneralSinkBin()
{
    m_tiler = gst_element_factory_make("nvmultistreamtiler", "sink-nvmultistreamtiler");
    GST_ASSERT(m_tiler);
    g_object_set(G_OBJECT(m_tiler), "rows", m_gstparams.tiler_rows, NULL);
    g_object_set(G_OBJECT(m_tiler), "columns", m_gstparams.tiler_cols, NULL);
    g_object_set(G_OBJECT(m_tiler), "width", m_gstparams.tiler_width, NULL);
    g_object_set(G_OBJECT(m_tiler), "height", m_gstparams.tiler_height, NULL);
    m_convert = gst_element_factory_make("nvvideoconvert", "video-convert");
    GST_ASSERT(m_convert);

    m_osd = gst_element_factory_make("nvdsosd", "sink-nvdsosd");
    GST_ASSERT(m_osd);
    m_tee = gst_element_factory_make("tee", "nvsink-tee");
    m_queue_display = gst_element_factory_make("queue", "nvtee-queue-display");

    m_sink = gst_element_factory_make("nveglglessink", "nv-sink");
    GST_ASSERT(m_sink);
    g_object_set(G_OBJECT(m_sink), "sync", TRUE, NULL);
    gst_bin_add_many(GST_BIN(m_pipeline), m_tiler, m_convert, m_osd, m_tee, m_queue_display, m_sink, NULL);

    if (!gst_element_link_many(m_tiler, m_convert, m_osd, m_tee, NULL))
    {
        gst_printerr("Could not link tiler, osd and sink\n");
    }

    if (!gst_element_link_many(m_queue_display, m_sink, NULL))
    {
        gst_printerr("Could not link tiler, osd and sink\n");
    }
    GstPad *sink_pad = gst_element_get_static_pad(m_queue_display, "sink");
    m_tee_display_pad = gst_element_get_request_pad(m_tee, "src_%u");
    if (!m_tee_display_pad)
    {
        g_printerr("%s:%d Unable to get request pads\n", __FILE__, __LINE__);
    }

    if (gst_pad_link(m_tee_display_pad, sink_pad) != GST_PAD_LINK_OK)
    {
        g_printerr("Unable to link tee and message converter\n");
        gst_object_unref(sink_pad);
    }

    gst_object_unref(sink_pad);

    {
        GstPad *osd_sink_pad = gst_element_get_static_pad(m_osd, "sink");
        GST_ASSERT(osd_sink_pad);
        gst_pad_add_probe(osd_sink_pad, GST_PAD_PROBE_TYPE_BUFFER, osd_yolo_sink_pad_buffer_probe,
                          reinterpret_cast<gpointer>(m_tiler), NULL);
        gst_object_unref(osd_sink_pad);
    }

    {
        GstPad *tiler_sink_pad = gst_element_get_static_pad(m_tiler, "sink");
        GST_ASSERT(tiler_sink_pad);
        gst_pad_add_probe(tiler_sink_pad, GST_PAD_PROBE_TYPE_BUFFER, tiler_sink_pad_buffer_probe,
                          reinterpret_cast<gpointer>(m_tiler), NULL);
        g_object_unref(tiler_sink_pad);
    }
    return m_sink;
}

void AppPipeline::linkMsgBroker()
{
    m_msgconv = gst_element_factory_make("nvmsgconv", "nvmsg-converter");
    m_msgbroker = gst_element_factory_make("nvmsgbroker", "nvmsg-broker");
    m_queue_msg = gst_element_factory_make("queue", "nvtee-queue-msg");

    if (!m_msgbroker || !m_msgconv || !m_tee || !m_queue_display || !m_queue_msg)
    {
        g_printerr("%s:%dOne element could not be created. Exiting.\n", __FILE__, __LINE__);
    }

    g_object_set(G_OBJECT(m_msgconv), "config", m_cloudParams.msg_config_path.c_str(), NULL);
    std::cout << "MSG2P-LIB dir = " << m_cloudParams.msg2p_lib << std::endl;
    g_object_set(G_OBJECT(m_msgconv), "msg2p-lib", m_cloudParams.msg2p_lib.c_str(), NULL);
    g_object_set(G_OBJECT(m_msgconv), "payload-type", m_cloudParams.schema_type, NULL);
    g_object_set(G_OBJECT(m_msgconv), "msg2p-newapi", m_cloudParams.msg2p_meta, NULL);
    g_object_set(G_OBJECT(m_msgconv), "frame-interval", m_cloudParams.frame_interval, NULL);

    g_object_set(G_OBJECT(m_msgbroker), "proto-lib", m_cloudParams.proto_lib.c_str(),
                 "conn-str", m_cloudParams.connection_str.c_str(), "sync", FALSE, NULL);

    g_object_set(G_OBJECT(m_msgbroker), "topic", m_cloudParams.topic.c_str(), NULL);

    gst_bin_add_many(GST_BIN(m_pipeline), m_queue_msg, m_msgconv, m_msgbroker, NULL);

    if (!gst_element_link_many(m_queue_msg, m_msgconv, m_msgbroker, NULL))
    {
        g_printerr("%s:%d Elements could not be linked \n", __FILE__, __LINE__);
    }

    GstPad *sink_pad = gst_element_get_static_pad(m_queue_msg, "sink");
    m_tee_msg_pad = gst_element_get_request_pad(m_tee, "src_%u");
    if (!m_tee_msg_pad)
    {
        g_printerr("Unable to get request pads\n");
    }

    if (gst_pad_link(m_tee_msg_pad, sink_pad) != GST_PAD_LINK_OK)
    {
        g_printerr("Unable to link tee and message converter\n");
        gst_object_unref(sink_pad);
    }

    gst_object_unref(sink_pad);
}