#include <stdio.h>
#include <gst/gst.h>
#include <glib.h>
#include <cuda.h>
#include <cuda_runtime_api.h>
#include "../samples/sample.h"
#include "app.h"

int main(int argc, char *argv[])
{
	// test_mp4(argc, argv);
	GMainLoop *loop = NULL;
	GstBus *bus = NULL;
	guint bus_watch_id;

	gst_init(&argc, &argv);
	loop = g_main_loop_new(NULL, FALSE);

	FaceApp app;
	app.add_video_source(argv[1], "video-1");
	app.add_video_source(argv[2], "video-2");
	app.linkToShowVideo();

	bus = gst_pipeline_get_bus(GST_PIPELINE(app.m_pipeline));
	GST_ASSERT(bus);
	bus_watch_id = gst_bus_add_watch(bus, bus_watch_callback, nullptr);

	gst_element_set_state(app.m_pipeline, GST_STATE_PLAYING);
	g_main_loop_run(loop);
	gst_element_set_state(app.m_pipeline, GST_STATE_NULL);
	g_source_remove(bus_watch_id);
	g_main_loop_unref(loop);
	return 0;
}
