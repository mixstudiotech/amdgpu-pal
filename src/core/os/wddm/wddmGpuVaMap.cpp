// Copyright (c) 2026. SPDX-License-Identifier: MIT
#include "core/os/wddm/wddmGpuVaMap.h"
#include "core/os/wddm/wddmResult.h"

namespace Pal::Wddm
{
Result MapGpuVaPolicy(GpuVaMapPolicyState* state,uint64_t logicalHint,uint64_t* callerPagingFence,
                      bool waitForPaging,const GpuVaMapPolicyCallbacks& callbacks)
{
    if(state==nullptr){return Result::ErrorInvalidPointer;}
    if(state->allocationCount==0){return Result::Success;}
    if(state->allocations==nullptr||callbacks.map==nullptr||
       (callerPagingFence==nullptr&&waitForPaging&&callbacks.waitPaging==nullptr))
    {return Result::ErrorInvalidPointer;}
    constexpr uint64_t ChunkBytes=0x8000000;
    const uint64_t base=logicalHint>state->gpuvmOffsetForSvm?logicalHint-state->gpuvmOffsetForSvm:logicalHint;
    Result result=Result::Success;
    for(uint32_t chunk=0;chunk<state->allocationCount;++chunk)
    {
        GpuVaMapPacket request;
        request.pagingQueue=state->pagingQueue;
        request.allocation=state->allocations[chunk];
        request.base=base+uint64_t(chunk)*ChunkBytes;
        request.minimum=state->primary.base;
        request.maximum=state->primary.base+state->primary.size;
        request.sizeInPages=(state->allocationCount>1&&chunk!=state->allocationCount-1)?ChunkBytes/4096:
            (state->size-uint64_t(chunk)*ChunkBytes)/4096;
        request.protection=state->readOnly?0:1;
        result=ConvertNativeStatus(callbacks.map(callbacks.context,&request));
        if(result!=Result::Success&&result!=Result::NotReady&&state->partition==0&&state->defaultVaRangeSplit)
        {
            request.minimum=state->defaultBackup.base;
            request.maximum=state->defaultBackup.base+state->defaultBackup.size;
            result=ConvertNativeStatus(callbacks.map(callbacks.context,&request));
            // These original branches require Result8. The authenticated raw
            // translator has no such output; no synthetic status alias is used.
            if(static_cast<int32_t>(result)==8&&base!=0)
            {
                const uint64_t backupMinimum=request.minimum;
                request.minimum=request.base;request.base=0;
                result=ConvertNativeStatus(callbacks.map(callbacks.context,&request));
                if(static_cast<int32_t>(result)==8)
                {
                    request.minimum=backupMinimum;
                    result=ConvertNativeStatus(callbacks.map(callbacks.context,&request));
                }
            }
        }
        if(result==Result::NotReady)
        {
            if(callerPagingFence!=nullptr)
            {
                if(*callerPagingFence<request.pagingFence){*callerPagingFence=request.pagingFence;}
                if(state->recordedPagingFence<request.pagingFence){state->recordedPagingFence=request.pagingFence;}
            }
            else if(waitForPaging){result=callbacks.waitPaging(callbacks.context,request.pagingFence);}
            else
            {
                if(state->recordedPagingFence<request.pagingFence){state->recordedPagingFence=request.pagingFence;}
                result=Result::Success;
            }
        }
        else if(result==Result::Success&&callerPagingFence!=nullptr){*callerPagingFence=0;}
        if(static_cast<int32_t>(result)>=0&&chunk==0)
        {state->gpuAddress=request.virtualAddress+state->gpuvmOffsetForSvm;}
    }
    return result;
}
}
