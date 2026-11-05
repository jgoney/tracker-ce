#include "Ft812Model.h"

#include <cstring>

void Ft812Model::getImgProps(uint32_t& addr, uint32_t& width, uint32_t& height)
{
    addr = 0;
    width = 0;
    height = 0;
}

void Ft812Model::readDataRAMG(uint8_t* imgData, uint32_t dataSize, uint32_t)
{
    if (imgData)
        std::memset(imgData, 0, dataSize);
}

void Ft812Model::readBackDataRAMG(uint8_t* imgData, uint32_t dataSize, uint32_t)
{
    if (imgData)
        std::memset(imgData, 0, dataSize);
}
