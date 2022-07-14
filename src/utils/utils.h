#ifndef UTILS_H
#define UTILS_H
#include <gst/gst.h>
#include <cassert>
#include <stdio.h>
#ifndef GST_ASSERT
#define GST_ASSERT(ans) assert_98dae521c1e67e8b70f66d14866fe14e((ans), __FILE__, __LINE__);
inline void assert_98dae521c1e67e8b70f66d14866fe14e(void* element, const char *file, int line)
{
    if (!element) {
        gst_printerr ("could not create element %s:%d\n", file, line);
        gst_object_unref(element);
        exit(-3);
    }
}
#endif // GST_ASSERT

#ifndef VTX_ASSERT
#define VTX_ASSERT assert
#endif

inline gboolean bus_watch_callback(GstBus *_bus, GstMessage *_msg, gpointer _uData)
{
    switch (GST_MESSAGE_TYPE(_msg))
    {
    case GST_MESSAGE_EOS:
        printf("GST_MESSAGE_EOS\r\n");
        break;
    case GST_MESSAGE_WARNING:
    {
        gchar *debug;
        GError *error;
        gst_message_parse_warning(_msg, &error, &debug);
        g_print("Warning: %s: %s\n", error->message, debug);
        g_free(debug);
        g_error_free(error);
        break;
    }
    case GST_MESSAGE_ERROR:
    {
        gchar *debug;
        GError *error;
        gst_message_parse_error(_msg, &error, &debug);
        g_printerr("Error: %s: %s\n", error->message, debug);
        g_free(debug);
        g_error_free(error);
        break;
    }
    default:
        printf(".");
        fflush(stdout);
        break;
    }
    return TRUE;
}

#endif