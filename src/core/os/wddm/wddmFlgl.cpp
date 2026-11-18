// Copyright (c) 2026. SPDX-License-Identifier: MIT
#include "wddmFlgl.h"
#include "wddmProperties.h"
#include "wddmWorkstation.h"
#include "wddmResult.h"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winternl.h>
#include <d3dkmthk.h>
#include <array>
#include <cstring>

namespace Pal::Wddm
{
static_assert(sizeof(FlglState)==12);
static_assert(sizeof(GlSyncConfig)==24);
namespace
{
void Put32(uint8* data,size_t offset,uint32 value) { std::memcpy(data+offset,&value,4); }
uint32 Get32(const uint8* data,size_t offset) { uint32 value;std::memcpy(&value,data+offset,4);return value; }
template<size_t N> std::array<uint8,N> Record(uint32 command,uint32 inputBytes,uint32 headerBytes)
{
    std::array<uint8,N> data={};
    Put32(data.data(),0,0x80);Put32(data.data(),4,0x10000);Put32(data.data(),8,0x2000000);
    Put32(data.data(),0x80,inputBytes);Put32(data.data(),0x84,headerBytes);Put32(data.data(),0x88,command);
    return data;
}
std::array<uint8,172> RouteQuery(uint32 display)
{
    auto packet=Record<172>(0x130035,16,16);Put32(packet.data(),0x8c,display);
    Put32(packet.data(),0x94,20);Put32(packet.data(),0x98,20);return packet;
}
}

Result ReplayFlglRecord(const FlglRoute& route,void* record,size_t bytes,const FlglCallbacks* callbacks)
{
    if (route.nativeEscapeDisabled) { return Result::Unsupported; }
    // All authenticated FLGL/connector methods fit the original2048-byte inline
    // envelope. This helper does not claim the unrelated dynamic-envelope path.
    if ((record==nullptr) || (bytes<76) || (bytes>1728)) { return Result::ErrorInvalidValue; }
    std::array<uint8,2048> packet={};
    Put32(packet.data(),0,2);Put32(packet.data(),4,0x10002);
    Put32(packet.data(),12,route.physicalAdapterIndex);Put32(packet.data(),72,uint32(bytes));
    std::memcpy(packet.data()+76,record,bytes);
    D3DKMT_ESCAPE request={};
    request.hAdapter=route.adapter;request.hDevice=route.device;
    request.pPrivateDriverData=packet.data();request.PrivateDriverDataSize=uint32(bytes+76);
    const int32_t status=((callbacks!=nullptr)&&(callbacks->escape!=nullptr))?
        callbacks->escape(callbacks->userData,&request):D3DKMTEscape(&request);
    const Result result=ConvertNativeStatus(status);
    if (result!=Result::Success) { return result; }
    std::memcpy(record,packet.data()+76,bytes);
    return Get32(static_cast<const uint8*>(record),72)==0?Result::Success:Result::ErrorUnknown;
}

uint32 Flgl::ControllerCount() const
{
    auto packet=Record<184>(0x110001,16,16);
    Put32(packet.data(),0x94,32);Put32(packet.data(),0x98,32);
    Replay(packet.data(),packet.size());
    return Get32(packet.data(),0xb0); // Original ignores the escape result.
}
void Flgl::UpdateSupport()
{
    if (ControllerCount()==0)
    { m_state.support=FlglSupport::NotAvailable;m_state.firmwareVersion=0;return; }
    auto packet=Record<176>(0x110052,16,16);
    Put32(packet.data(),0x94,24);Put32(packet.data(),0x98,24);
    const Result result=Replay(packet.data(),packet.size());
    if ((Get32(packet.data(),72)==7)||(result!=Result::Success))
    { m_state.support=FlglSupport::NotConnected;m_state.firmwareVersion=0; }
    else { m_state.support=FlglSupport::Available;m_state.firmwareVersion=packet[0xa1]; }
}
Result Flgl::EnumerateRoutes(uint8* routes,uint32* count) const
{
    const uint32 capacity=*count;
    const uint32 copyBytes=capacity*20;
    const uint32 tail=capacity!=0?copyBytes-20:0;
    const uint32 bytes=tail+224;
    auto* packet=static_cast<uint8*>(m_allocator.Alloc(Util::AllocInfo(bytes,8,false,Util::AllocInternal)));
    if (packet==nullptr) { *count=0;return Result::ErrorOutOfMemory; }
    std::memset(packet,0,bytes);
    Put32(packet,0,0x80);Put32(packet,4,0x10000);Put32(packet,8,0x2000000);
    Put32(packet,0x80,52);Put32(packet,0x84,52);Put32(packet,0x88,0x110059);
    Put32(packet,0xb8,tail+36);Put32(packet,0xbc,tail+36);
    const Result result=Replay(packet,bytes);
    std::memcpy(routes,packet+0xcc,copyBytes);
    *count=Get32(packet,0xc0);
    m_allocator.Free(Util::FreeInfo(packet));
    return result;
}
// Native 38a250 returns the last escape result (the route enumeration's when there are no routes).
Result Flgl::UpdateGenLock()
{
    m_state.u32All &= ~5u;
    std::array<uint8,120> routes={};uint32 count=6;
    Result result=EnumerateRoutes(routes.data(),&count);
    for (uint32 i=0;i<count;++i)
    {
        auto packet=RouteQuery(Get32(routes.data(),i*20+16));
        result=Replay(packet.data(),packet.size());
        const uint32 flags=Get32(packet.data(),0x9c);
        if ((flags&2)!=0) { m_state.u32All |=5;break; }
        if ((flags&1)!=0) { m_state.u32All |=1; }
    }
    return result;
}
Result Flgl::InitializeFromCapabilities(Result capabilityResult,uint32 caps)
{
    m_state.support=FlglSupport::NotAvailable;
    Result result=capabilityResult;
    if ((result==Result::Success)&&((caps&1)!=0))
    {
        UpdateSupport();
        if (m_state.support==FlglSupport::Available) { result=UpdateGenLock(); }
    }
    return result;
}
Result Flgl::QueryState(FlglState* state)
{
    if (state==nullptr) { return Result::ErrorInvalidPointer; }
    uint32 caps=0;Result result;
    if (m_callbacks.workstationCaps!=nullptr) { result=m_callbacks.workstationCaps(m_callbacks.userData,&caps); }
    else
    {
        PhysicalProperties properties;
        result=ConvertNativeStatus(QueryPhysicalProperties(m_route.adapter,&properties));
        if (result==Result::Success) { caps=DecodeWorkstationCapabilities(properties); }
    }
    if (result==Result::Success)
    {
        if ((caps&1)!=0)
        { UpdateSupport();if (m_state.support!=FlglSupport::NotAvailable) { UpdateGenLock(); } }
        *state=m_state;
    }
    return result;
}
Result Flgl::Control(uint32 control) const
{
    auto packet=Record<192>(0x110055,52,52);
    Put32(packet.data(),0x94,36);Put32(packet.data(),0x98,64);Put32(packet.data(),0xa0,control);
    const uint32 count=ControllerCount();Result result=Result::Success;
    for (uint32 i=0;i<count;++i)
    {
        Put32(packet.data(),0x8c,i);result=Replay(packet.data(),packet.size());
        if (result!=Result::Success) { break; }
    }
    return result;
}
Result Flgl::SetSyncConfiguration(const GlSyncConfig& config)
{
    auto packet=Record<192>(0x110055,52,52);
    Put32(packet.data(),0x94,36);Put32(packet.data(),0x98,config.validMask);
    Put32(packet.data(),0x9c,config.syncDelay);Put32(packet.data(),0xa0,config.framelockCntlVector);
    Put32(packet.data(),0xa4,config.signalSource);packet[0xa8]=config.sampleRate;
    packet[0xa9]=0; // The original deliberately drops the caller's syncField.
    packet[0xaa]=config.triggerEdge;packet[0xab]=config.scanRateCoeff;
    Put32(packet.data(),0xac,config.sigGenFrequency);
    const uint32 count=ControllerCount();Result result=Result::Success;
    for (uint32 i=0;i<count;++i)
    {
        Put32(packet.data(),0x8c,i);result=Replay(packet.data(),packet.size());
        if (result!=Result::Success) { break; }
    }
    return result;
}
Result Flgl::GetConfig(uint32 mask,uint8* config36) const
{
    auto packet=Record<192>(0x110054,20,16);
    Put32(packet.data(),0x94,mask);Put32(packet.data(),0x98,36);Put32(packet.data(),0x9c,36);
    const Result result=Replay(packet.data(),packet.size());
    std::memcpy(config36,packet.data()+0x9c,36); // Copied even when the escape failed.
    return result;
}
Result Flgl::GetSyncConfiguration(GlSyncConfig* config) const
{
    if (config==nullptr) { return Result::ErrorInvalidPointer; }
    std::array<uint8,36> data={};const Result result=GetConfig(config->validMask,data.data());
    if (result==Result::Success)
    {
        config->syncDelay=Get32(data.data(),8);config->framelockCntlVector=Get32(data.data(),12);
        config->signalSource=Get32(data.data(),16);config->sampleRate=data[20];config->syncField=data[21];
        config->triggerEdge=data[22];config->scanRateCoeff=data[23];config->sigGenFrequency=Get32(data.data(),24);
    }
    return result;
}
bool Flgl::FrameLockEnabled() const
{
    std::array<uint8,36> data={};
    return GetConfig(64,data.data())==Result::Success && ((Get32(data.data(),12)&1)!=0);
}
Result Flgl::SetFrameLock(bool enable)
{
    if ((m_state.support==FlglSupport::NotAvailable)||(m_state.support==FlglSupport::NotConnected)) { return Result::Unsupported; }
    const bool enabled=(m_state.u32All&2)!=0;
    if (enable!=enabled) { Control(enable?17:18); }
    const bool readback=FrameLockEnabled();m_state.u32All=(m_state.u32All&~2u)|(uint32(readback)<<1);
    return Result((int(enable)^int(!readback))-1);
}
Result Flgl::SetGenLock(bool enable)
{
    std::array<uint8,120> routes={};uint32 count=6;EnumerateRoutes(routes.data(),&count);
    for (uint32 i=0;i<count;++i)
    {
        const uint32 display=Get32(routes.data(),i*20+16);auto query=RouteQuery(display);
        Replay(query.data(),query.size());uint32 flags=Get32(query.data(),0x9c);
        if ((!enable&&((flags&1)!=0))||(enable&&(flags==0)))
        {
            auto set=Record<176>(0x130036,36,16);Put32(set.data(),0x8c,display);
            std::memcpy(set.data()+0x94,query.data()+0x98,20);Put32(set.data(),0x98,uint32(enable));
            Put32(set.data(),0xa8,4);Replay(set.data(),set.size());
        }
    }
    const bool enabled=FrameLockEnabled();m_state.u32All=(m_state.u32All&~2u)|(uint32(enabled)<<1);
    const Result result=Result((int(enable)^int(!enabled))-1);
    UpdateSupport();if (m_state.support!=FlglSupport::NotAvailable) { UpdateGenLock(); }
    return result;
}
Result Flgl::ResetFrameCounter() const
{
    if ((m_state.support==FlglSupport::NotAvailable)||(m_state.support==FlglSupport::NotConnected)) { return Result::Unsupported; }
    return (m_state.u32All&4)!=0?Control(4):Result::ErrorUnavailable;
}
Result Flgl::FetchCounter(uint64* value,bool* reset) const
{
    auto packet=Record<180>(0x110058,20,20);Put32(packet.data(),0x94,uint32(value!=nullptr));
    Put32(packet.data(),0x98,24);Put32(packet.data(),0x9c,24);
    const Result result=Replay(packet.data(),packet.size());
    if (result==Result::Success)
    {
        if (value!=nullptr) { std::memcpy(value,packet.data()+0xac,8); }
        if (reset!=nullptr) { *reset=Get32(packet.data(),0xa4)==1; }
    }
    return result;
}
Result Flgl::GetFrameCounter(uint64* value,bool* reset) const
{
    if ((value==nullptr)||(reset==nullptr)) { return Result::ErrorInvalidPointer; }
    if ((m_state.support==FlglSupport::NotAvailable)||(m_state.support==FlglSupport::NotConnected)) { return Result::Unsupported; }
    *reset=false;const Result result=FetchCounter(value,reset);
    if ((result==Result::Success)&&*reset) { Control(8);FetchCounter(value,reset); }
    return result;
}
Result Flgl::GetFrameCounterResetStatus(bool* reset) const
{
    if (reset==nullptr) { return Result::ErrorInvalidPointer; }
    if ((m_state.support==FlglSupport::NotAvailable)||(m_state.support==FlglSupport::NotConnected)) { return Result::Unsupported; }
    const Result result=FetchCounter(nullptr,reset);
    if ((result==Result::Success)&&*reset) { Control(8); }
    return result;
}
}
