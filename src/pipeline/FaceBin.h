#ifndef FACEBIN_H_764b8ce325dd4743054ac8de
#define FACEBIN_H_764b8ce325dd4743054ac8de
#include "NvInferBinBase.h"
#include "NvInferBinConfigBase.h"
#include "params.h"
#include <curl/curl.h>
#include "rapidjson/document.h"
#include "rapidjson/writer.h"
#include "rapidjson/stringbuffer.h"
#include <nvds_obj_encode.h>

using namespace rapidjson;

class NvInferFaceBinConfig : public NvInferBinConfigBase
{
public:
    NvInferFaceBinConfig(std::string pgie, std::string sgie, std::string aligner)
    {
        pgie_config_path = pgie;
        sgie_config_path = sgie;
        aligner_config_path = aligner;
    }
    ~NvInferFaceBinConfig() = default;
    std::string aligner_config_path;
};

class NvInferFaceBin : public NvInferBinBase
{
public:
    NvInferFaceBin(std::shared_ptr<NvInferFaceBinConfig> configs);
    ~NvInferFaceBin();

    void createInferBin() override;
    void createDetectBin();
    void acquireUserData(user_callback_data * callback_data);
    void setMsgBrokerConfig() override;
    void attachProbe() override;
    static void sgie_output_callback(GstBuffer *buf,
                                     NvDsInferNetworkInfo *network_info,
                                     NvDsInferLayerInfo *layers_info,
                                     guint num_layers,
                                     guint batch_size,
                                     gpointer user_data);
    static GstPadProbeReturn osd_sink_pad_buffer_probe(GstPad *pad, GstPadProbeInfo *info, gpointer _udata);
    static GstPadProbeReturn tiler_sink_pad_buffer_probe(GstPad *pad, GstPadProbeInfo *info, gpointer _udata);
    static GstPadProbeReturn pgie_src_pad_buffer_probe(GstPad *pad, GstPadProbeInfo *info, gpointer _udata);
    // static GstPadProbeReturn sgie_src_pad_buffer_probe(GstPad *pad, GstPadProbeInfo *info, gpointer _udata);

    GstElement *aligner = NULL;
    user_callback_data *m_user_callback_data;

    NvDsObjEncCtxHandle m_obj_ctx_handle;
};

#endif