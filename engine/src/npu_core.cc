#include "npu_core.h"

// select_core_mask maps a configured npu_core to an RKNN core mask.
// -1 means "all cores": let the RKNN runtime schedule across every core,
// which load-balances multiple concurrent engine processes. On a
// single-core part RKNN_NPU_CORE_0_1_2 resolves to the one core present.
rknn_core_mask select_core_mask(int npu_core) {
    switch (npu_core) {
        case 0:  return RKNN_NPU_CORE_0;
        case 1:  return RKNN_NPU_CORE_1;
        case 2:  return RKNN_NPU_CORE_2;
        default: return RKNN_NPU_CORE_0_1_2;
    }
}
