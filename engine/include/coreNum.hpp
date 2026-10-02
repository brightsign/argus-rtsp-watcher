// Vendored from leafqycc/rknn-cpp-Multithreading
// (https://github.com/leafqycc/rknn-cpp-Multithreading), licensed under the
// Apache License, Version 2.0 (http://www.apache.org/licenses/LICENSE-2.0).
#ifndef CORENUM_H
#define CORENUM_H

#include <stdio.h>

#include "rknn_api.h"

const int RK3588 = 3;

// 设置模型需要绑定的核心
// Set the core of the model that needs to be bound
int get_core_num()
{
    static int core_num = 0;
    static std::mutex mtx;

    std::lock_guard<std::mutex> lock(mtx);

    int temp = core_num % RK3588;
    core_num++;
    return temp;
}
#endif