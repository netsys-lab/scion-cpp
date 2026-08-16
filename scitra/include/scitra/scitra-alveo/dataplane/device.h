#pragma once

#include "vitis_net_p4/vitisnetp4_common.h"

#include <stdint.h>


struct Device
{
    int sysfile;
#if MOCK_IO
    #define MEMORY_SIZE 0x14000
    uint8_t* memory;
#endif
};

int device_open(struct Device* dev, const char* sys_file);
void device_close(struct Device* dev);
uint32_t device_read32(struct Device* dev, uint32_t address);
void device_write32(struct Device* dev, uint32_t address, uint32_t data);
