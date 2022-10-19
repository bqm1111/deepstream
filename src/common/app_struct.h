#ifndef APP_STRUCT_H
#define APP_STRUCT_H
#include "params.h"
#include <curl/curl.h>
#include "kafka_producer.h"
#include "tracker.h"

struct user_callback_data
{
    CURL *curl;
    gchar *session_id;
    std::vector<std::string> video_name;
    KafkaProducer *kafka_producer;
    tracker *trackers = nullptr;
    gchar* timestamp;
    float face_feature_confidence_threshold;
    bool save_crop_img;

    int muxer_output_width;
    int muxer_output_height;
    int muxer_batch_size;
    int muxer_buffer_pool_size;
    int muxer_nvbuf_memory_type;
    bool muxer_live_source;
    
    int tiler_rows;
    int tiler_cols;
    int tiler_width;
    int tiler_height;

    std::string metadata_topic;
    std::string visual_topic;
    std::string connection_str;
    std::string curl_address;
};


#endif