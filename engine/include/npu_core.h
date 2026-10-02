#pragma once
#include "rknn_api.h"

// select_core_mask maps a configured npu_core value to an RKNN core mask.
rknn_core_mask select_core_mask(int npu_core);
