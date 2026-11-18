// Copyright (c) 2026. SPDX-License-Identifier: MIT
#include "wddmFlgl.h"
#include <array>
#include <cstring>

namespace Pal::Wddm
{
namespace
{
void Put32(uint8* data,size_t offset,uint32 value){std::memcpy(data+offset,&value,4);}
uint32 Get32(const uint8* data,size_t offset){uint32 value;std::memcpy(&value,data+offset,4);return value;}
DisplayConnectorType Type(uint32 raw)
{
    switch(raw)
    {
    case 1:return DisplayConnectorType::Vga;
    case 2:return DisplayConnectorType::DviD;
    case 3:return DisplayConnectorType::DviI;
    case 8:case 9:return DisplayConnectorType::Hdmi;
    case 10:return DisplayConnectorType::Dp;
    case 11:return DisplayConnectorType::Edp;
    case 12:return DisplayConnectorType::Minidp;
    default:return DisplayConnectorType::Unknown;
    }
}
}
Result QueryDisplayConnectors(const FlglRoute& route,uint32* count,DisplayConnectorProperties* connectors,
                               const FlglCallbacks* callbacks)
{
    std::array<uint8,1728> packet={};
    Put32(packet.data(),0,0x80);Put32(packet.data(),4,0x10000);Put32(packet.data(),8,0x2000000);
    Put32(packet.data(),0x4c,0xbadcaffe);Put32(packet.data(),0x80,16);Put32(packet.data(),0x84,16);
    Put32(packet.data(),0x88,0x110069);Put32(packet.data(),0x94,0x628);
    const Result result=ReplayFlglRecord(route,packet.data(),packet.size(),callbacks);
    if(result==Result::Success)
    {
        uint32 total=0;
        for(uint32 row=0;row<Get32(packet.data(),0x9c);++row)
        {
            const size_t first=0xac+size_t(row)*0x184;
            const uint32 items=Get32(packet.data(),first-4);
            if(connectors!=nullptr)for(uint32 i=0;i<items;++i)
            {connectors[total+i].type=Type(Get32(packet.data(),first+size_t(i)*36));}
            total+=items;
        }
        // Native313be0 does not treat the incoming count as a capacity. A null
        // output array is its count-only query. The count pointer is required.
        *count=total;
    }
    return result;
}
}
