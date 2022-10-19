#ifndef PARAMS_H_8971a2c3da276ee5d7f01820
#define PARAMS_H_8971a2c3da276ee5d7f01820

#include <gst/gst.h>
#include <cassert>
#include <stdio.h>
#include <iostream>
#include <vector>
#include <nvdsmeta_schema.h>
#include "gstnvdsmeta.h"
#include "nvdspreprocess_meta.h" // must bellow gstnvdsmeta.h
#include "gstnvdsinfer.h"        // must bellow gstnvdsmeta.h
#include <chrono>
#ifndef NVDS_OBJ_USER_META_MOT
#define NVDS_OBJ_USER_META_MOT (nvds_get_user_meta_type("NVIDIA.NVINFER.OBJ_USER_META_MOT"))
#endif

#ifndef NVDS_OBJ_USER_META_FACE
#define NVDS_OBJ_USER_META_FACE (nvds_get_user_meta_type("NVIDIA.NVINFER.OBJ_USER_META_FACE"))
#endif

#ifndef NUM_FACEMARK
#define NUM_FACEMARK 5
#endif

#ifndef FACE_CLASS_ID
#define FACE_CLASS_ID 1000
#endif

#ifndef FEATURE_SIZE
#define FEATURE_SIZE 512
#endif

#ifndef MAX_DISPLAY_LEN
#define MAX_DISPLAY_LEN 64
#endif
#define MAX_TIME_STAMP_LEN 32
#define PGIE_CLASS_ID_VEHICLE 2
#define PGIE_CLASS_ID_PERSON 0

#ifndef FACEID_PGIE_CONFIG_PATH
#define FACEID_PGIE_CONFIG_PATH "../configs/faceid/faceid_primary.txt"
#endif
#ifndef FACEID_ALIGN_CONFIG_PATH
#define FACEID_ALIGN_CONFIG_PATH "../configs/faceid/faceid_align_config.txt"
#endif
#ifndef FACEID_SGIE_CONFIG_PATH
#define FACEID_SGIE_CONFIG_PATH "../configs/faceid/faceid_secondary.txt"
#endif

#ifndef MOT_PGIE_CONFIG_PATH
#define MOT_PGIE_CONFIG_PATH "../configs/faceid/mot_primary.txt"
#endif

#ifndef MOT_SGIE_CONFIG_PATH
#define MOT_SGIE_CONFIG_PATH "../configs/faceid/mot_sgie.txt"
#endif

#ifndef KAFKA_MSG2P_LIB
#define KAFKA_MSG2P_LIB "src/nvmsgconv/libnvmsgconv.so"
#endif

#ifndef KAFKA_PROTO_LIB
#define KAFKA_PROTO_LIB "src/kafka_protocol_adaptor/libnvds_kafka_proto.so"
#endif

#define POST_TRACK_SCORE 1.0
#define SESSION_ID_LENGTH 37


struct alignas(float) Detection
{
    float bbox[4]; // x1 y1 x2 y2
    float class_confidence;
    float landmark[10];
};

// typedef NvDsFaceAlignMeta NvDsFacePointsMetaData;

struct FacePose
{
    float yaw, pitch, roll;
};
enum NvDsFaceMetaStage
{
    EMPTY = -1,
    DETECTED = 0,
    DROPPED,
    ALIGNED,
    MASKPOSED,
    FEATURED,
    NAMED,
};

struct NvDsFaceMetaData
{
    NvDsFaceMetaStage stage;

    /* Assigned in detection */
    float faceMark[2 * NUM_FACEMARK];

    /* Assigned in maskpose */
    bool hasMask;
    FacePose pose;

    /* Assigned in alignment.
     * This face will be formed into the aligned_index in aligned_tensor
     */
    GstNvDsPreProcessBatchMeta *aligned_tensor = nullptr;
    int aligned_index;

    /* Assigned in feature extraction */
    float feature[FEATURE_SIZE];

    /* Assigned in the naming process */
    // std::string name = "";
    // void* customFaceMeta = nullptr;
    const char *name;
    int staff_id;
    float naming_score;
};

typedef struct FaceEventMsgData
{
    gchar *feature;
} FaceEventMsgData;

typedef struct NvDsFaceMsgData
{
    double timestamp;
    gint frameId;
    gchar* cameraId;
    NvDsRect bbox;
    double confidence_score;
    gchar *name;
    gchar *staff_id;
    gchar *feature;
    gchar *encoded_img;
} NvDsFaceMsgData;

typedef struct NvDsMOTMsgData
{
    double timestamp;
    gint frameId;
    gchar* cameraId;
    NvDsRect bbox;
    int track_id;
    gchar *embedding;
} NvDsMOTMsgData;

struct XFaceVisualMsg
{
    gchar* timestamp;
    gint frameId;
    gchar *cameraId;
    gchar *sessionId;
    gchar *full_img;
    gint width;
    gint height;
    gint num_channel;
};

struct XFaceMetaMsg
{
    gchar* timestamp;
    gint frameId;
    gchar *sessionId;
    gchar *cameraId;
    gint num_face_obj;
    gint num_mot_obj;
    NvDsFaceMsgData **face_meta_list;
    NvDsMOTMsgData **mot_meta_list;
};

typedef struct NvDsMOTMetaData
{
    gchar *feature;
} NvDsMOTMetaData;

struct SinkPerfStruct
{
    bool start_perf_measurement = false;
    std::chrono::high_resolution_clock::time_point last_tick = std::chrono::high_resolution_clock::now();
    gdouble total_time = 0;
    gdouble num_ticks = 0;
};

#endif