/*
 * SPDX-FileCopyrightText: Copyright (c) 2023-2024 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <stdint.h>
#include <stdio.h>

#include <iostream>
#include <random>

void getFromCin(void* ptr, uint32_t size)
{
    unsigned char* cPtr = reinterpret_cast<unsigned char*>(ptr);
    for (uint32_t counter = 0; counter < size; ++counter)
        *(cPtr + counter) = (unsigned char)getchar();
}

uint8_t getUint8t()
{
    return (unsigned char)getchar();
}

void getFromUrand(void* buffer, uint32_t num_bytes)
{
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<uint8_t> dis(0, 255);

    uint8_t* data = static_cast<uint8_t*>(buffer);
    for (size_t i = 0; i < num_bytes; ++i)
    {
        data[i] = dis(gen);
    }

    std::cout << std::dec << std::endl;
}

uint8_t getUint8tUrand()
{
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<uint8_t> dis(
        0, std::numeric_limits<uint8_t>::max());

    return dis(gen);
}

bool getBoolCin()
{
    unsigned char in = getchar();
    return in < (0xFF >> 1);
}

void getFromCin(void* ptr, uint32_t size, bool useUrandom)
{
    return useUrandom ? getFromUrand(ptr, size) : getFromCin(ptr, size);
}

uint8_t getUint8t(bool useUrandom)
{
    return useUrandom ? getUint8tUrand() : getUint8t();
}
