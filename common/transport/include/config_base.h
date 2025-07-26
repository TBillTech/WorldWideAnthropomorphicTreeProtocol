#pragma once
#include <string>
#include <string_view>
#include <deque>
#include <unordered_map>
#include <vector>
#include <array>
#include <ngtcp2/ngtcp2.h>
#include <iostream>
#include <ngtcp2/ngtcp2_crypto.h>

#include "network.h"
#include "util.h"
#include "request.h"


extern std::ofstream keylog_file;
