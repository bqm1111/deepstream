#include "FaceBin.h"
#include <algorithm>
#include <cmath>
#include <nvdsinfer_custom_impl.h>
#include "utils.h"

gpointer user_copy_facemark_meta(gpointer data, gpointer user_data)
{
    NvDsUserMeta *user_meta = reinterpret_cast<NvDsUserMeta *>(data);
    NvDsFaceMetaData *facemark_meta_data_ptr = reinterpret_cast<NvDsFaceMetaData *>(
        user_meta->user_meta_data);
    NvDsFaceMetaData *new_facemark_meta_data_ptr = reinterpret_cast<NvDsFaceMetaData *>(
        g_memdup(facemark_meta_data_ptr, sizeof(NvDsFaceMetaData)));
    return reinterpret_cast<gpointer>(new_facemark_meta_data_ptr);
}

/**
 * @brief implement NvDsMetaReleaseFun
 *
 * @param data
 * @param user_data
 */
void user_release_facemark_data(gpointer data, gpointer user_data)
{
    NvDsUserMeta *user_meta = reinterpret_cast<NvDsUserMeta *>(data);
    NvDsFaceMetaData *facemark_meta_data_ptr = reinterpret_cast<NvDsFaceMetaData *>(
        user_meta->user_meta_data);
    delete facemark_meta_data_ptr;
}

static inline bool cmp(Detection &a, Detection &b)
{
    return a.class_confidence > b.class_confidence;
}

static inline float iou(float lbox[4], float rbox[4])
{
    float interBox[] = {
        std::max(lbox[0], rbox[0]), // left
        std::min(lbox[2], rbox[2]), // right
        std::max(lbox[1], rbox[1]), // top
        std::min(lbox[3], rbox[3]), // bottom
    };

    if (interBox[2] > interBox[3] || interBox[0] > interBox[1])
        return 0.0f;

    float interBoxS = (interBox[1] - interBox[0]) * (interBox[3] - interBox[2]);
    return interBoxS /
           ((lbox[2] - lbox[0]) * (lbox[3] - lbox[1]) +
            (rbox[2] - rbox[0]) * (rbox[3] - rbox[1]) - interBoxS + 0.000001f);
}

static void nms(std::vector<Detection> &res, float *output, float post_cluster_thresh = 0.7, float iou_threshold = 0.4)
{
    std::vector<Detection> dets;
    for (int i = 0; i < output[0]; i++)
    {
        if (output[15 * i + 1 + 4] <= post_cluster_thresh)
            continue;
        Detection det;
        memcpy(&det, &output[15 * i + 1], sizeof(Detection));
        dets.push_back(det);
    }
    std::sort(dets.begin(), dets.end(), cmp);
    for (size_t m = 0; m < dets.size(); ++m)
    {
        auto &item = dets[m];
        res.push_back(item);
        // std::cout << item.class_confidence << " bbox " << item.bbox[0] << ", " << item.bbox[1] << ", " << item.bbox[2] << ", " << item.bbox[3] << std::endl;
        for (size_t n = m + 1; n < dets.size(); ++n)
        {
            if (iou(item.bbox, dets[n].bbox) > iou_threshold)
            {
                dets.erase(dets.begin() + n);
                --n;
            }
        }
    }
}

static void generate_ts_rfc3339(char *buf, int buf_size)
{
    time_t tloc;
    struct tm tm_log;
    struct timespec ts;
    char strmsec[6]; //.nnnZ\0

    clock_gettime(CLOCK_REALTIME, &ts);
    memcpy(&tloc, (void *)(&ts.tv_sec), sizeof(time_t));
    gmtime_r(&tloc, &tm_log);
    strftime(buf, buf_size, "%Y-%m-%dT%H:%M:%S", &tm_log);
    int ms = ts.tv_nsec / 1000000;
    g_snprintf(strmsec, sizeof(strmsec), ".%.3dZ", ms);
    strncat(buf, strmsec, buf_size);
}

static void
generate_event_msg_meta(gpointer data, NvDsObjectMeta *obj_meta)
{
    NvDsFaceMetaData *faceMeta = NULL;
    for (NvDsMetaList *l_user = obj_meta->obj_user_meta_list; l_user != NULL; l_user = l_user->next)
    {
        NvDsUserMeta *user_meta = reinterpret_cast<NvDsUserMeta *>(l_user->data);
        if (user_meta->base_meta.meta_type == (NvDsMetaType)NVDS_OBJ_USER_META_FACE)
        {
            faceMeta = reinterpret_cast<NvDsFaceMetaData *>(user_meta->user_meta_data);
        }
    }
    NvDsEventMsgMeta *meta = (NvDsEventMsgMeta *)data;
    meta->bbox.top = obj_meta->detector_bbox_info.org_bbox_coords.top;
    meta->bbox.left = obj_meta->detector_bbox_info.org_bbox_coords.left;
    meta->bbox.width = obj_meta->detector_bbox_info.org_bbox_coords.width;
    meta->bbox.height = obj_meta->detector_bbox_info.org_bbox_coords.height;

    meta->ts = (gchar *)g_malloc0(MAX_TIME_STAMP_LEN + 1);
    meta->objectId = (gchar *)g_malloc0(MAX_LABEL_SIZE);

    strncpy(meta->objectId, obj_meta->obj_label, MAX_LABEL_SIZE);
    generate_ts_rfc3339(meta->ts, MAX_TIME_STAMP_LEN);
    if (obj_meta->class_id == FACE_CLASS_ID)
    {
        FaceEventMsgData *obj = (FaceEventMsgData *)g_malloc0(sizeof(FaceEventMsgData));

        obj->feature = g_strdup((gchar *)b64encode(faceMeta->feature, FEATURE_SIZE));
        meta->objType = NVDS_OBJECT_TYPE_FACE;
        meta->extMsg = obj;
        meta->extMsgSize = sizeof(FaceEventMsgData);
    }
}

static gpointer meta_copy_func(gpointer data, gpointer user_data)
{
    NvDsUserMeta *user_meta = (NvDsUserMeta *)data;
    NvDsEventMsgMeta *srcMeta = (NvDsEventMsgMeta *)user_meta->user_meta_data;
    NvDsEventMsgMeta *dstMeta = NULL;
    dstMeta = (NvDsEventMsgMeta *)g_memdup(srcMeta, sizeof(NvDsEventMsgMeta));
    if (srcMeta->ts)
        dstMeta->ts = g_strdup(srcMeta->ts);

    if (srcMeta->objectId)
    {
        dstMeta->objectId = g_strdup(srcMeta->objectId);
    }
    if (srcMeta->extMsgSize > 0)
    {
        if (srcMeta->objType == NVDS_OBJECT_TYPE_FACE)
        {
            FaceEventMsgData *srcObj = (FaceEventMsgData *)srcMeta->extMsg;
            FaceEventMsgData *obj = (FaceEventMsgData *)g_malloc0(sizeof(FaceEventMsgData));
            if (srcObj->feature)
            {
                obj->feature = g_strdup(srcObj->feature);
            }
            dstMeta->extMsg = obj;
            dstMeta->extMsgSize = sizeof(FaceEventMsgData);
        }
    }
    return dstMeta;
}
static void meta_free_func(gpointer data, gpointer user_data)
{
    NvDsUserMeta *user_meta = (NvDsUserMeta *)data;
    NvDsEventMsgMeta *srcMeta = (NvDsEventMsgMeta *)user_meta->user_meta_data;
    g_free(srcMeta->ts);
    if (srcMeta->objectId)
    {
        g_free(srcMeta->objectId);
    }
    if (srcMeta->extMsgSize > 0)
    {
        if (srcMeta->objType == NVDS_OBJECT_TYPE_FACE)
        {
            FaceEventMsgData *obj = (FaceEventMsgData *)srcMeta->extMsg;
            if (obj->feature)
            {
                g_free(obj->feature);
            }
        }
        g_free(srcMeta->extMsg);
        srcMeta->extMsgSize = 0;
    }
    g_free(user_meta->user_meta_data);
    user_meta->user_meta_data = NULL;
}
GstPadProbeReturn NvInferFaceBin::osd_sink_pad_callback(GstPad *pad, GstPadProbeInfo *info, gpointer _udata)
{
    GstBuffer *buf = reinterpret_cast<GstBuffer *>(info->data);
    GST_ASSERT(buf);
    NvDsBatchMeta *batch_meta = gst_buffer_get_nvds_batch_meta(buf);
    GST_ASSERT(batch_meta);
    GstElement *tiler = reinterpret_cast<GstElement *>(_udata);
    GST_ASSERT(tiler);
    gint tiler_rows, tiler_cols, tiler_width, tiler_height;
    g_object_get(tiler, "rows", &tiler_rows, "columns", &tiler_cols, "width", &tiler_width, "height", &tiler_height, NULL);

    guint num_rects = 0;
    // loop through each frame in batch
    NvDsMetaList *l_frame = NULL;
    NvDsMetaList *l_obj = NULL;
    NvDsMetaList *l_user = NULL;
    for (l_frame = batch_meta->frame_meta_list; l_frame != NULL; l_frame = l_frame->next)
    {
        NvDsFrameMeta *frame_meta = reinterpret_cast<NvDsFrameMeta *>(l_frame->data);
        float muxer_output_height = frame_meta->pipeline_height;
        float muxer_output_width = frame_meta->pipeline_width;
        // translate from batch_id to the position of this frame in tiler
        int tiler_col = frame_meta->batch_id / tiler_cols;
        int tiler_row = frame_meta->batch_id % tiler_cols;
        int offset_x = tiler_col * tiler_width / tiler_cols;
        int offset_y = tiler_row * tiler_height / tiler_rows;
        // loop through each object in frame data

        for (l_obj = frame_meta->obj_meta_list; l_obj != NULL; l_obj = l_obj->next)
        {
            NvDsObjectMeta *obj_meta = reinterpret_cast<NvDsObjectMeta *>(l_obj->data);
            if (obj_meta->class_id == FACE_CLASS_ID)
            {
                num_rects++;
                // draw landmark here
                for (l_user = obj_meta->obj_user_meta_list; l_user != NULL; l_user = l_user->next)
                {
                    NvDsUserMeta *user_meta = reinterpret_cast<NvDsUserMeta *>(l_user->data);
                    if (user_meta->base_meta.meta_type != (NvDsMetaType)NVDS_OBJ_USER_META_FACE)
                    {
                        continue;
                    }
                    NvDsFaceMetaData *faceMeta = static_cast<NvDsFaceMetaData *>(user_meta->user_meta_data);
                    NvDsDisplayMeta *display_meta = nvds_acquire_display_meta_from_pool(batch_meta);
                    display_meta->num_circles = NUM_FACEMARK;
                    for (int j = 0; j < NUM_FACEMARK; j++)
                    {
                        float x = faceMeta->faceMark[2 * j] * tiler_width / muxer_output_width;
                        float y = faceMeta->faceMark[2 * j + 1] * tiler_height / muxer_output_height;
                        display_meta->circle_params[j].xc = static_cast<unsigned int>(x);
                        display_meta->circle_params[j].yc = static_cast<unsigned int>(y);
                        display_meta->circle_params[j].radius = 4;
                        display_meta->circle_params[j].circle_color.red = 1.0;
                        display_meta->circle_params[j].circle_color.green = 1.0;
                        display_meta->circle_params[j].circle_color.blue = 0.0;
                        display_meta->circle_params[j].circle_color.alpha = 1.0;
                        display_meta->circle_params[j].has_bg_color = 1;
                        display_meta->circle_params[j].bg_color.red = 1.0;
                        display_meta->circle_params[j].bg_color.green = 0.0;
                        display_meta->circle_params[j].bg_color.blue = 0.0;
                        display_meta->circle_params[j].bg_color.alpha = 1.0;
                    }
                    nvds_add_display_meta_to_frame(frame_meta, display_meta);
                }

                // ================== EVENT MESSAGE DATA ========================
                NvDsEventMsgMeta *msg_meta = (NvDsEventMsgMeta *)g_malloc0(sizeof(NvDsEventMsgMeta));

                generate_event_msg_meta(msg_meta, obj_meta);

                NvDsUserMeta *user_event_meta = nvds_acquire_user_meta_from_pool(batch_meta);
                if (user_event_meta)
                {
                    user_event_meta->user_meta_data = (void *)msg_meta;
                    user_event_meta->base_meta.meta_type = NVDS_EVENT_MSG_META;
                    user_event_meta->base_meta.copy_func = (NvDsMetaCopyFunc)meta_copy_func;
                    user_event_meta->base_meta.release_func = (NvDsMetaReleaseFunc)meta_free_func;
                    nvds_add_user_meta_to_frame(frame_meta, user_event_meta);
                }
                else
                {
                    g_print("Error in attaching event meta to buffer\n");
                }
            }
        }

        NvDsDisplayMeta *display_meta = nvds_acquire_display_meta_from_pool(batch_meta);
        display_meta->num_labels = 1;
        NvOSD_TextParams *txt_params = &display_meta->text_params[0];
        txt_params->display_text = reinterpret_cast<char *>(g_malloc0(MAX_DISPLAY_LEN));
        int offset = snprintf(txt_params->display_text, MAX_DISPLAY_LEN, "Frame Number = %d Number of faces = %d", frame_meta->frame_num, num_rects);
        // g_print("Frame Number = %d Number of faces = %d\n", frame_meta->frame_num, num_rects);
        txt_params->x_offset = 10;
        txt_params->y_offset = 12;
        txt_params->font_params.font_name = "Serif";
        txt_params->font_params.font_size = 10;
        txt_params->font_params.font_color.red = 1.0;
        txt_params->font_params.font_color.green = 1.0;
        txt_params->font_params.font_color.blue = 1.0;
        txt_params->font_params.font_color.alpha = 1.0;
        txt_params->set_bg_clr = 1;
        txt_params->text_bg_clr.red = 0.0;
        txt_params->text_bg_clr.green = 0.0;
        txt_params->text_bg_clr.blue = 0.0;
        txt_params->text_bg_clr.alpha = 1.0;
        nvds_add_display_meta_to_frame(frame_meta, display_meta);
    }
    return GST_PAD_PROBE_OK;
}

GstPadProbeReturn NvInferFaceBin::pgie_src_pad_buffer_probe(GstPad *pad, GstPadProbeInfo *info, gpointer _udata)
{
    GstBuffer *buf = reinterpret_cast<GstBuffer *>(info->data);
    NvDsBatchMeta *batch_meta = gst_buffer_get_nvds_batch_meta(buf);

    // FIXME: should parse from config file
    NvDsInferParseDetectionParams detectionParams;
    detectionParams.numClassesConfigured = 1;
    detectionParams.perClassPreclusterThreshold = {0.1};
    detectionParams.perClassPostclusterThreshold = {0.7};

    NvDsMetaList *l_frame = NULL;
    NvDsMetaList *l_obj = NULL;
    NvDsMetaList *l_raw = NULL;
    for (l_frame = batch_meta->frame_meta_list; l_frame != NULL; l_frame = l_frame->next)
    {
        NvDsFrameMeta *frame_meta = reinterpret_cast<NvDsFrameMeta *>(l_frame->data);
        // raw output when enable output-tensor-meta
        float muxer_output_height = frame_meta->pipeline_height;
        float muxer_output_width = frame_meta->pipeline_width;
        for (l_raw = frame_meta->frame_user_meta_list; l_raw != NULL; l_raw = l_raw->next)
        {
            NvDsUserMeta *user_meta = reinterpret_cast<NvDsUserMeta *>(l_raw->data);
            if (user_meta->base_meta.meta_type != NVDSINFER_TENSOR_OUTPUT_META)
            {
                continue;
            }

            /* convert to tensor metadata */
            NvDsInferTensorMeta *meta = reinterpret_cast<NvDsInferTensorMeta *>(user_meta->user_meta_data);
            float pgie_net_height = meta->network_info.height;
            float pgie_net_width = meta->network_info.width;

            VTX_ASSERT(meta->num_output_layers == 1); // we only have one output layer
            for (unsigned int i = 0; i < meta->num_output_layers; i++)
            {
                NvDsInferLayerInfo *info = &meta->output_layers_info[i];
                info->buffer = meta->out_buf_ptrs_host[i];
                if (meta->out_buf_ptrs_dev[i])
                {
                    cudaMemcpy(meta->out_buf_ptrs_host[i], meta->out_buf_ptrs_dev[i],
                               info->inferDims.numElements * 4, cudaMemcpyDeviceToHost);
                }
            }

            /* Parse output tensor, similar to NvDsInferParseCustomRetinaface  */
            std::vector<NvDsInferLayerInfo> outputLayersInfo(meta->output_layers_info, meta->output_layers_info + meta->num_output_layers);
            NvDsInferLayerInfo outputLayerInfo = outputLayersInfo.at(0);
            // std::vector < NvDsInferObjectDetectionInfo > objectList;
            float *output = (float *)outputLayerInfo.buffer;

            std::vector<Detection> res;
            nms(res, output, detectionParams.perClassPostclusterThreshold[0]);
            /* Iterate final rectangules and attach result into frame's obj_meta_list */
            for (const auto &obj : res)
            {
                NvDsObjectMeta *obj_meta = nvds_acquire_obj_meta_from_pool(batch_meta);

                // FIXME: a `meta` can produce more than once `obj`. Hence cannot set obj_meta->unique_component_id = meta->unique_id;
                obj_meta->unique_component_id = meta->unique_id;
                obj_meta->confidence = obj.class_confidence;

                // untracked object. Set tracking_id to -1
                obj_meta->object_id = UNTRACKED_OBJECT_ID;
                obj_meta->class_id = FACE_CLASS_ID; // only have one class

                /* retrieve bouding box */
                float scale_x = muxer_output_width / pgie_net_width;
                float scale_y = muxer_output_height / pgie_net_height;

                obj_meta->detector_bbox_info.org_bbox_coords.left = obj.bbox[0];
                obj_meta->detector_bbox_info.org_bbox_coords.top = obj.bbox[1];
                obj_meta->detector_bbox_info.org_bbox_coords.width = obj.bbox[2] - obj.bbox[0];
                obj_meta->detector_bbox_info.org_bbox_coords.height = obj.bbox[3] - obj.bbox[1];

                obj_meta->detector_bbox_info.org_bbox_coords.left *= scale_x;
                obj_meta->detector_bbox_info.org_bbox_coords.top *= scale_y;
                obj_meta->detector_bbox_info.org_bbox_coords.width *= scale_x;
                obj_meta->detector_bbox_info.org_bbox_coords.height *= scale_y;

                /* for nvdosd */
                NvOSD_RectParams &rect_params = obj_meta->rect_params;
                NvOSD_TextParams &text_params = obj_meta->text_params;
                rect_params.left = obj_meta->detector_bbox_info.org_bbox_coords.left;
                rect_params.top = obj_meta->detector_bbox_info.org_bbox_coords.top;
                rect_params.width = obj_meta->detector_bbox_info.org_bbox_coords.width;
                rect_params.height = obj_meta->detector_bbox_info.org_bbox_coords.height;
                rect_params.border_width = 3;
                rect_params.has_bg_color = 0;
                rect_params.border_color = (NvOSD_ColorParams){1, 0, 0, 1};

                // store landmark in obj_user_meta_list
                NvDsFaceMetaData *face_meta_ptr = new NvDsFaceMetaData();
                for (int j = 0; j < 2 * NUM_FACEMARK; j += 2)
                {
                    face_meta_ptr->stage = NvDsFaceMetaStage::DETECTED;
                    face_meta_ptr->faceMark[j] = obj.landmark[j];
                    face_meta_ptr->faceMark[j + 1] = obj.landmark[j + 1];
                    face_meta_ptr->faceMark[j] *= scale_x;
                    face_meta_ptr->faceMark[j + 1] *= scale_y;
                }

                NvDsUserMeta *user_meta = nvds_acquire_user_meta_from_pool(batch_meta);
                user_meta->user_meta_data = static_cast<void *>(face_meta_ptr);
                NvDsMetaType user_meta_type = (NvDsMetaType)NVDS_OBJ_USER_META_FACE;
                user_meta->base_meta.meta_type = user_meta_type;
                user_meta->base_meta.copy_func = (NvDsMetaCopyFunc)user_copy_facemark_meta;
                user_meta->base_meta.release_func = (NvDsMetaReleaseFunc)user_release_facemark_data;
                nvds_add_user_meta_to_obj(obj_meta, user_meta);
                nvds_add_obj_meta_to_frame(frame_meta, obj_meta, NULL);
            }
        }
    }
    return GST_PAD_PROBE_OK;
}

GstPadProbeReturn NvInferFaceBin::sgie_src_pad_buffer_probe(GstPad *pad, GstPadProbeInfo *info, gpointer _udata)
{
    GstBuffer *buf = reinterpret_cast<GstBuffer *>(info->data);
    NvDsBatchMeta *batchmeta = gst_buffer_get_nvds_batch_meta(buf);

    NvDsMetaList *l_frame = NULL;
    NvDsMetaList *l_obj = NULL;
    NvDsMetaList *l_user = NULL;

    NvDsInferLayerInfo *output_layer_info;

    int tensor_output_meta_count = 0;
    for (l_frame = batchmeta->frame_meta_list; l_frame != NULL; l_frame = l_frame->next)
    {
        NvDsFrameMeta *frame_meta = reinterpret_cast<NvDsFrameMeta *>(l_frame->data);
        int cnt = 0;
        for (l_user = frame_meta->frame_user_meta_list; l_user != NULL; l_user = l_user->next)
        {
            NvDsUserMeta *user_meta = reinterpret_cast<NvDsUserMeta *>(l_user->data);
            if (user_meta->base_meta.meta_type != NVDSINFER_TENSOR_OUTPUT_META)
                continue;
            cnt++;
            printf("\n%s:%d tensor_output_meta_count=%d \n", __FILE__, __LINE__, tensor_output_meta_count++);
            NvDsInferTensorMeta *tensor_meta = reinterpret_cast<NvDsInferTensorMeta *>(user_meta->user_meta_data);
            // std::cout << "Total num output layer = " << tensor_meta->num_output_layers << std::endl;
            for (int i = 0; i < tensor_meta->num_output_layers; i++)
            {
                output_layer_info = &tensor_meta->output_layers_info[i];
                output_layer_info->buffer = tensor_meta->out_buf_ptrs_host[i];
                if (tensor_meta->out_buf_ptrs_dev[i])
                {
                    cudaMemcpy(tensor_meta->out_buf_ptrs_host[i], tensor_meta->out_buf_ptrs_dev[i], output_layer_info->inferDims.numElements * 4, cudaMemcpyDeviceToHost);
                }
            }
        }

        for (l_obj = frame_meta->obj_meta_list; l_obj != NULL; l_obj = l_obj->next)
        {
            NvDsObjectMeta *obj_meta = reinterpret_cast<NvDsObjectMeta *>(l_obj->data);
            NvDsInferTensorMeta *tensor_meta = reinterpret_cast<NvDsInferTensorMeta *>(obj_meta->obj_user_meta_list);
            if (obj_meta->class_id == FACE_CLASS_ID)
            {
                for (NvDsMetaList *l_user = obj_meta->obj_user_meta_list; l_user != NULL; l_user = l_user->next)
                {
                    NvDsUserMeta *user_meta = reinterpret_cast<NvDsUserMeta *>(l_user->data);
                    if (user_meta->base_meta.meta_type == (NvDsMetaType)NVDS_OBJ_USER_META_FACE)
                    {
                        NvDsFaceMetaData *faceMeta = reinterpret_cast<NvDsFaceMetaData *>(user_meta->user_meta_data);
                        // if (faceMeta->stage == NvDsFaceMetaStage::EMPTY)
                        //     continue;
                        // if (faceMeta->stage == NvDsFaceMetaStage::FEATURED)
                        //     continue;
                        // if (faceMeta->stage != NvDsFaceMetaStage::ALIGNED) {
                        //     printf(" %s:%d ERROR in feature, found an NVDS_OBJ_USER_META_FACE with stage = %d\n", __FILE__, __LINE__, faceMeta->stage);
                        //     exit(1);
                        // }
                        // faceMeta->stage = NvDsFaceMetaStage::FEATURED;
                        // // float *cur_feature = reinterpret_cast<float *>(output_layer_info->buffer) +
                        // //                      faceMeta->aligned_index * output_layer_info->inferDims.numElements;
                        // printf("\n%s:%d algined_index=%d", __FILE__, __LINE__, faceMeta->aligned_index);
                        // printf("\n%s:%d output_layer_info = ", __FILE__, __LINE__);
                        // for(int jjj = 0; jjj < output_layer_info->inferDims.numDims; jjj++){
                        //     printf(" %d ", output_layer_info->inferDims.d[jjj]);
                        // }
                        // printf("\n");
                        memcpy(faceMeta->feature, reinterpret_cast<float *>(output_layer_info->buffer) + faceMeta->aligned_index * output_layer_info->inferDims.numElements, FEATURE_SIZE * sizeof(float));
                        // for(int i = 0; i < FEATURE_SIZE; i++)
                        // {
                        //     std::cout << faceMeta->feature[i] << std::endl;
                        // }
                        // printf("\n%s:%d algined_index=%d output_layer_info->buffer=%p\n", __FILE__, __LINE__, faceMeta->aligned_index, output_layer_info->buffer);
                    }
                }
            }
        }
    }
    return GST_PAD_PROBE_OK;
}

void NvInferFaceBin::sgie_output_callback(GstBuffer *buf,
                                          NvDsInferNetworkInfo *network_info,
                                          NvDsInferLayerInfo *layers_info,
                                          guint num_layers,
                                          guint batch_size,
                                          gpointer user_data)
{
    user_feature_callback_data_t *callback_data = reinterpret_cast<user_feature_callback_data_t *>(user_data);

    /* Find the only output layer */
    NvDsInferLayerInfo *output_layer_info;
    NvDsInferLayerInfo *input_layer_info;
    for (int i = 0; i < num_layers; i++)
    {
        NvDsInferLayerInfo *info = &layers_info[i];
        if (info->isInput)
        {
            input_layer_info = info;
        }
        else
        {
            output_layer_info = info;
            // TODO: the info also include input tensor, which is the 3x112x112 input. COuld be use for something.
        }
    }

    /* Assign feature to NvDsFaceMetaData */
    NvDsBatchMeta *batch_meta = gst_buffer_get_nvds_batch_meta(buf);
    for (NvDsMetaList *l_frame = batch_meta->frame_meta_list; l_frame != NULL; l_frame = l_frame->next)
    {
        NvDsFrameMeta *frame_meta = reinterpret_cast<NvDsFrameMeta *>(l_frame->data);
        for (NvDsMetaList *l_obj = frame_meta->obj_meta_list; l_obj != NULL; l_obj = l_obj->next)
        {
            NvDsObjectMeta *obj_meta = reinterpret_cast<NvDsObjectMeta *>(l_obj->data);
            if (FACE_CLASS_ID != obj_meta->class_id)
                continue;
            for (NvDsMetaList *l_user = obj_meta->obj_user_meta_list; l_user != NULL; l_user = l_user->next)
            {
                NvDsUserMeta *user_meta = reinterpret_cast<NvDsUserMeta *>(l_user->data);
                if (user_meta->base_meta.meta_type == (NvDsMetaType)NVDS_OBJ_USER_META_FACE)
                {
                    NvDsFaceMetaData *faceMeta = reinterpret_cast<NvDsFaceMetaData *>(user_meta->user_meta_data);

                    const int feature_size = output_layer_info->inferDims.numElements;
                    float *cur_feature = reinterpret_cast<float *>(output_layer_info->buffer) +
                                         faceMeta->aligned_index * feature_size;
                    memcpy(faceMeta->feature, cur_feature, feature_size * sizeof(float));
                    float norm = 0;
                    for(int i = 0; i < FEATURE_SIZE; i++)
                    {
                        norm += cur_feature[i] * cur_feature[i];
                    }
                    norm = std::sqrt(norm);
                    // std::cout << "VECTOR NORM = " << norm << std::endl;
                    // Send HTTP request
                    CURL *curl = callback_data->curl;
                    const char *data = gen_body(1, b64encode(cur_feature, FEATURE_SIZE));
                    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, data);
                    
                    // request over HTTP/2, using the same connection!
                    CURLcode res = curl_easy_perform(curl);
                    if (res != CURLE_OK)
                    {
                        fprintf(stderr, "curl_easy_perform() failed: %s\n", curl_easy_strerror(res));
                        break;
                    }

#ifdef DEBUG_FACE_SGIE
                    /* save input of sgie */
                    {
                        auto tensor_meta = faceMeta->aligned_tensor->tensor_meta;
                        int one_tensor_size = tensor_meta->buffer_size / tensor_meta->tensor_shape[0];
                        gpointer h_tensor = g_malloc(one_tensor_size);
                        g_assert(cudaSuccess == cudaMemcpy(h_tensor,
                                                           tensor_meta->raw_tensor_buffer + faceMeta->aligned_index * one_tensor_size,
                                                           one_tensor_size, cudaMemcpyDeviceToHost));

                        // frame-number_stream-number_object-number_object-type_widthxheight.jpg
                        const int MAX_STR_LEN = 1024;
                        char *filelocation = (char *)g_malloc0(MAX_STR_LEN);
                        snprintf(filelocation, MAX_STR_LEN - 1, "sgie_input_%d_%d_%d_roi_l%.f_t%.f_w%.f_h%.f.bin",
                                 frame_meta->frame_num,
                                 frame_meta->source_id,
                                 faceMeta->aligned_index,
                                 obj_meta->detector_bbox_info.org_bbox_coords.left,
                                 obj_meta->detector_bbox_info.org_bbox_coords.top,
                                 obj_meta->detector_bbox_info.org_bbox_coords.width,
                                 obj_meta->detector_bbox_info.org_bbox_coords.height);

                        FILE *file = fopen(filelocation, "w");
                        if (!file)
                        {
                            g_printerr("Could not open file '%s' for writing:%s\n", filelocation, strerror(errno));
                        }
                        else
                        {
                            size_t written = fwrite(h_tensor, sizeof(float), one_tensor_size / sizeof(float), file);
                            if (written != one_tensor_size / sizeof(float))
                            {
                                g_printerr("Error occurred at writing time to '%s'!\n", filelocation);
                                exit(1);
                            }
                        }
                        fclose(file);

                        g_free(h_tensor);
                    }

                    /* save output of sgie */
                    {
                        const int MAX_STR_LEN = 1024;
                        char *filelocation = (char *)g_malloc0(MAX_STR_LEN);
                        snprintf(filelocation, MAX_STR_LEN - 1, "sgie_output_%d_%d_%d_roi_l%.f_t%.f_w%.f_h%.f.bin",
                                 frame_meta->frame_num,
                                 frame_meta->source_id,
                                 faceMeta->aligned_index,
                                 obj_meta->detector_bbox_info.org_bbox_coords.left,
                                 obj_meta->detector_bbox_info.org_bbox_coords.top,
                                 obj_meta->detector_bbox_info.org_bbox_coords.width,
                                 obj_meta->detector_bbox_info.org_bbox_coords.height);

                        FILE *file = fopen(filelocation, "w");
                        if (!file)
                        {
                            g_printerr("Could not open file '%s' for writing:%s\n", filelocation, strerror(errno));
                        }
                        else
                        {
                            size_t written = fwrite(faceMeta->feature, sizeof(float), feature_size, file);
                            if (written != feature_size)
                            {
                                g_printerr("Error occurred at writing time to '%s'!\n", filelocation);
                                exit(1);
                            }
                        }
                        fclose(file);
                    }
#endif // DEBUG_FACE_SGIE
                }
            }
        }
    }
    callback_data->tensor_count++;
}

GstPadProbeReturn NvInferFaceBin::tiler_sink_pad_buffer_probe(GstPad *pad, GstPadProbeInfo *info, gpointer _udata)
{
    GstBuffer *buf = reinterpret_cast<GstBuffer *>(info->data);
    GST_ASSERT(buf);
    NvDsBatchMeta *batch_meta = gst_buffer_get_nvds_batch_meta(buf);
    GST_ASSERT(batch_meta);

    GstElement *tiler = reinterpret_cast<GstElement *>(_udata);
    GST_ASSERT(tiler);
    gint tiler_rows, tiler_cols, tiler_width, tiler_height = 0;
    g_object_get(tiler, "rows", &tiler_rows, "columns", &tiler_cols, "width", &tiler_width, "height", &tiler_height, NULL);
    assert(tiler_height != 0);

    NvDsMetaList *l_frame = NULL;
    NvDsMetaList *l_obj = NULL;
    NvDsMetaList *l_user = NULL;

    for (l_frame = batch_meta->frame_meta_list; l_frame != NULL; l_frame = l_frame->next)
    {
        NvDsFrameMeta *frame_meta = reinterpret_cast<NvDsFrameMeta *>(l_frame->data);
        float muxer_output_height = frame_meta->pipeline_height;
        float muxer_output_width = frame_meta->pipeline_width;
        // translate from batch_id to the position of this frame in tiler
        int tiler_col = frame_meta->batch_id % tiler_cols;
        int tiler_row = frame_meta->batch_id / tiler_cols;
        int offset_x = tiler_col * tiler_width / tiler_cols;
        int offset_y = tiler_row * tiler_height / tiler_rows;
        // g_print("in tiler_sink_pad_buffer_probe batch_id = %d, the tiler offset = %d, %d\n", frame_meta->batch_id, offset_x, offset_y);
        // loop through each object in frame data
        for (l_obj = frame_meta->obj_meta_list; l_obj != NULL; l_obj = l_obj->next)
        {
            NvDsObjectMeta *obj_meta = reinterpret_cast<NvDsObjectMeta *>(l_obj->data);
            if (obj_meta->class_id == FACE_CLASS_ID)
            {
                for (l_user = obj_meta->obj_user_meta_list; l_user != NULL; l_user = l_user->next)
                {
                    NvDsUserMeta *user_meta = reinterpret_cast<NvDsUserMeta *>(l_user->data);
                    if (user_meta->base_meta.meta_type != (NvDsMetaType)NVDS_OBJ_USER_META_FACE)
                    {
                        continue;
                    }

                    NvDsFaceMetaData *faceMeta = static_cast<NvDsFaceMetaData *>(user_meta->user_meta_data);
                    // scale the landmark data base on tiler
                    for (int j = 0; j < NUM_FACEMARK; j++)
                    {
                        faceMeta->faceMark[2 * j] = faceMeta->faceMark[2 * j] / tiler_cols + offset_x;
                        faceMeta->faceMark[2 * j + 1] = faceMeta->faceMark[2 * j + 1] / tiler_rows + offset_y;
                    }
                }
            }
        }
    }
    return GST_PAD_PROBE_OK;
}