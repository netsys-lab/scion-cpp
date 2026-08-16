#include "scitra/scitra-alveo/dataplane/device.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>


int device_open(struct Device* dev, const char* sys_file)
{
    if ((dev->sysfile = open(sys_file, O_RDWR | O_SYNC)) < 0)
    {
        fprintf(stderr, "Error opening sysfile: %s\n", strerror(errno));
        return -1;
    }
    else
    {
    #if MOCK_IO
        dev->memory = calloc(MEMORY_SIZE, 1);
        if (!dev->memory) {
            fprintf(stderr, "Memory allocation failed\n");
            exit(-1);
        }
    #endif
        return 0;
    }
}

void device_close(struct Device* dev)
{
    if (dev->sysfile >= 0) {
        close(dev->sysfile);
        dev->sysfile = -1;
    }
#if MOCK_IO
    if (dev->memory) {
        free(dev->memory);
        dev->memory = NULL;
    }
#endif
}

uint32_t device_read32(struct Device* dev, uint32_t address)
{
    #ifdef _DEBUG
    fprintf(stderr, "Read value at offset 0x%08x: ", address);
    #endif

#if MOCK_IO
    if (address & 0x03) {
        fprintf(stderr, "Address not aligned\n");
        exit(-1);
    }
    if (address >= MEMORY_SIZE) {
        fprintf(stderr, "Insufficent mock memory\n");
        exit(-1);
    }
    uint32_t data = *(uint32_t*)(dev->memory + (address & -0x03));
#else
    off_t addr = (off_t)address;
    off_t offset = addr & (-4096);
    off_t rem = addr & 0xFFF;
    size_t length = 4096;

    void* region = mmap(0, length, PROT_READ , MAP_SHARED, dev->sysfile, offset);
    if (region == MAP_FAILED)
    {
        fprintf(stderr, "Error calling mmap: mapping failed\n");
        exit(-1);
    }
    void* virtual = region + rem;
    uint32_t data = *((uint32_t *)virtual);

    if(munmap(region, length) < 0)
    {
        fprintf(stderr, "Error calling munmap: %s\n", strerror(errno));
        exit(-1);
    }
#endif

    #ifdef _DEBUG
    fprintf(stderr, "0x%08x\n", data);
    #endif
    return data;
}

void device_write32(struct Device* dev, uint32_t address, uint32_t data)
{
    #ifdef _DEBUG
    fprintf(stderr, "Write to offset 0x%08x (0x%08x): ", address, data);
    #endif

#if MOCK_IO
    if (address & 0x03) {
        fprintf(stderr, "Address not aligned\n");
        exit(-1);
    }
    if (address >= MEMORY_SIZE) {
        fprintf(stderr, "Insufficent mock memory\n");
        exit(-1);
    }
    *(uint32_t*)(dev->memory + (address & -0x03)) = data;
#else
    off_t addr = (off_t)address;
    off_t offset = addr & (-4096);
    off_t rem = addr & 0xFFF;
    size_t length = 4096;

    void* region = mmap(0, length, PROT_WRITE, MAP_SHARED, dev->sysfile, offset);
    if (region == MAP_FAILED)
    {
        fprintf(stderr, "Error calling mmap: mapping failed\n");
        exit(-1);
    }
    void* virtual = region + rem;
    *((uint32_t *)virtual) = data;

    if(munmap(region, length) < 0)
    {
        fprintf(stderr, "Error calling munmap: %s\n", strerror(errno));
        exit(-1);
    }
#endif

    #ifdef _DEBUG
    fprintf(stderr, "OK\n");
    #endif
}
