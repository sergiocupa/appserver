#pragma once

#include "appserver.h"

Element* hls_prepare_video(Message* message);
Element* hls_stream_video(Message* message);
Element* dash_stream_video(Message* message);
Element* convert_file(Message* message);

