#include "config_base.h"
#include "communication.h"
#include <fstream>

std::ofstream keylog_file;

Config config{};

StreamIdentifier IDLE_stream = {true, true, {}, 0};
pair<StreamIdentifier, Request> IDLE_stream_listener = make_pair(IDLE_stream, Request());
