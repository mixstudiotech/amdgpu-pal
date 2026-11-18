# Native PAL objects and the AMD WDDM transport are built into the same PAL archive.
target_sources(pal PRIVATE
    wddmDevice.cpp wddmDevice.h
    wddmVa.cpp
    wddmSvm.cpp
    wddmDeviceState.cpp
    wddmExecution.cpp wddmExecution.h
    wddmApplicationProfile.cpp wddmApplicationProfile.h
    wddmEscape.cpp wddmEscape.h
    wddmDeviceControls.cpp wddmDeviceControls.h
    wddmDisplay.cpp wddmDisplay.h
    wddmFlgl.cpp wddmFlgl.h wddmConnectors.cpp
    wddmSppProfile.cpp wddmSppProfile.h
    wddmClock.cpp wddmClock.h
    wddmClockMode.cpp wddmClockMode.h
    wddmPower.cpp wddmPower.h
    wddmDriverVersion.cpp wddmDriverVersion.h
    wddmQueueLimit.cpp wddmQueueLimit.h
    wddmWorkstation.cpp wddmWorkstation.h
    wddmStaticVmid.cpp wddmStaticVmid.h
    wddmDmaUploadRing.cpp wddmDmaUploadRing.h
    wddmScreen.cpp wddmScreen.h
    wddmScreenPolicy.cpp wddmScreenPolicy.h
    wddmFence.cpp wddmFence.h
    wddmGpuMemory.cpp wddmGpuMemory.h
    wddmMemoryRecords.cpp wddmMemoryRecords.h
    wddmGpuVaMap.cpp wddmGpuVaMap.h wddmGpuVaMapPacket.h
    wddmImage.h
    wddmPlatform.cpp wddmPlatform.h
    wddmQueue.cpp wddmQueue.h
    wddmGang.cpp wddmGang.h
    wddmSubmit.cpp wddmSubmit.h
    wddmQueueIdle.cpp wddmQueueIdle.h
    wddmEventWait.cpp wddmEventWait.h
    wddmCpuWait.cpp wddmCpuWait.h
    wddmQueueSemaphore.cpp
    wddmSparse.cpp wddmSparse.h
    wddmSharedMemory.cpp wddmSharedMemory.h
    wddmSharedResource.cpp wddmSharedResource.h
    wddmExternalImage.cpp wddmExternalImage.h
    wddmTransport.cpp wddmTransport.h wddmPrivate.h
    wddmProperties.cpp wddmProperties.h
    wddmSwapChain.cpp wddmSwapChain.h
    wddmPresent.cpp wddmPresent.h
    wddmOglPresent.cpp wddmOglPresent.h
    wddmPresentScheduler.cpp wddmPresentScheduler.h
    wddmFullscreen.cpp wddmFullscreen.h
    wddmPrivateScreen.cpp wddmPrivateScreen.h
    wddmVirtualDisplay.cpp wddmVirtualDisplay.h
)
target_link_libraries(pal PUBLIC gdi32 dxgi d3d12 user32 dwmapi advapi32)
