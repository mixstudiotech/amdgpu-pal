// Copyright (c) 2026. SPDX-License-Identifier: MIT
#include "core/os/wddm/wddmPlatform.h"
#include "core/os/wddm/wddmDevice.h"
#include "core/os/wddm/wddmDisplay.h"
#include "core/os/wddm/wddmResult.h"
#include "core/os/wddm/wddmScreen.h"
#include "core/os/wddm/wddmScreenPolicy.h"

#include <cstdlib>
#include <windows.h>
#include <winternl.h>
#include <d3dkmthk.h>

namespace Pal
{
size_t Platform::GetSize()
{
    return sizeof(Wddm::Platform);
}
Platform *Platform::CreateInstance(const PlatformCreateInfo &info, const Util::AllocCallbacks &callbacks,
                                   void *placement)
{
    return PAL_PLACEMENT_NEW(placement) Wddm::Platform(info, callbacks);
}
} // namespace Pal

namespace Pal::Wddm
{
Result Platform::ConnectToOsInterface()
{
    std::vector<AdapterInfo> adapters;
    Result result = ConvertStatus(Transport::Enumerate(&adapters));
    // Native 300fd7..301041: the OS-interface setup opens or creates the DirectDraw exclusive-mode
    // mutexes and fails platform creation with ErrorUnknown when either is unavailable.
    if (result == Result::Success)
    {
        result = m_exclusiveMode.Init();
    }
    return result;
}

Result Platform::ReQueryDevices()
{
    TearDownDevices();
    std::vector<AdapterInfo> adapters;
    Result result = ConvertStatus(Transport::Enumerate(&adapters));
    if (result != Result::Success)
    {
        return result;
    }
    for (const auto &adapter : adapters)
    {
        if ((m_deviceCount == MaxDevices) || !Transport::IsVerifiedAdapter(adapter))
        {
            continue;
        }
        Device *device = nullptr;
        result = Device::Create(this, m_settingsPath, &device, adapter);
        if (result != Result::Success)
        {
            TearDownDevices();
            return result;
        }
        m_pDevice[m_deviceCount++] = device;
    }
    return (m_deviceCount > 0) ? Result::Success : Result::ErrorIncompatibleDevice;
}

Result Platform::ReQueryScreens(uint32 *count, void *storage[MaxScreens], IScreen *screens[MaxScreens])
{
    if (screens != nullptr)
    {
        for (uint32 index = 0; index < MaxScreens; ++index) { screens[index] = nullptr; }
    }
    if (count == nullptr)
    {
        return Result::ErrorInvalidPointer;
    }
    *count = 0;
    // Shared EnumerateDevices queries topology with both arrays null. It must
    // not construct caller-owned objects or change any display settings.
    if ((storage == nullptr) != (screens == nullptr))
    {
        return Result::ErrorInvalidPointer;
    }
    if (storage != nullptr)
    {
        for (uint32 index = 0; index < MaxScreens; ++index)
        {
            if (storage[index] == nullptr) { return Result::ErrorInvalidPointer; }
        }
    }
    std::vector<ScreenRecord> records;
    LONG win32Status = ERROR_SUCCESS;
    const Result result = QueryActiveScreenRecords(&records, &win32Status);
    // Native 3016c6..3016cb: ERROR_ACCESS_DENIED from the display configuration sets Platform+0x27bc.
    if (win32Status == ERROR_ACCESS_DENIED) { m_noDisplayAccess = true; }
    if (result != Result::Success)
    {
        // Device enumeration must remain usable from service/SSH sessions
        // which cannot access a desktop. Public object queries preserve the
        // actual failure instead of manufacturing an empty successful list.
        if ((storage == nullptr) && ((result == Result::ErrorUnavailable) || (result == Result::Unsupported)))
        {
            for (uint32 gpu = 0; gpu < m_deviceCount; ++gpu)
                { static_cast<Device*>(m_pDevice[gpu])->UpdateAttachedScreenCount(0); }
            return Result::Success;
        }
        return result;
    }
    std::vector<ScreenRecord> associated;
    const IDevice* owners[MaxScreens] = {};
    uint32 attachedCounts[MaxDevices] = {};
    ScreenAdapterAssociation adapters[MaxDevices] = {};
    for (uint32 gpu = 0; gpu < m_deviceCount; ++gpu)
    {
        const auto& adapter = static_cast<Device*>(m_pDevice[gpu])->Adapter();
        adapters[gpu] = { adapter.luidLow, adapter.luidHigh, m_pDevice[gpu] };
    }
    for (uint32 screen = 0; screen < records.size(); ++screen)
    {
        // Native 301480..3014d7 joins source LUID and requires a PAL Device.
        const uint32 gpu = FindScreenAdapter(records[screen], adapters, m_deviceCount);
        if (gpu != m_deviceCount)
        {
            // Native 3014b6: the source adapter object gets flag 8 (active display) in every query.
            static_cast<Device*>(m_pDevice[gpu])->MarkActiveDisplay();
            if (associated.size() == MaxScreens) { return Result::ErrorInvalidMemorySize; }
            owners[associated.size()] = m_pDevice[gpu];
            associated.push_back(records[screen]);
            ++attachedCounts[gpu];
        }
    }
    for (uint32 gpu = 0; gpu < m_deviceCount; ++gpu)
        { static_cast<Device*>(m_pDevice[gpu])->UpdateAttachedScreenCount(attachedCounts[gpu]); }
    if (storage != nullptr)
    {
        // Native 3015d8: every screen refreshes its colour state (36aba0); a failure tears that screen down,
        // destroys the screens already returned (301706..30173d) and fails the query.
        for (uint32 screen = 0; screen < associated.size(); ++screen)
        {
            auto* object = PAL_PLACEMENT_NEW(storage[screen]) Screen(associated[screen], owners[screen]);
            const Result refreshed = object->RefreshColorStateForEnumeration();
            if (refreshed != Result::Success)
            {
                object->Destroy();
                for (uint32 created = 0; created < screen; ++created)
                {
                    screens[created]->Destroy();
                    screens[created] = nullptr;
                }
                return refreshed;
            }
            screens[screen] = object;
        }
    }
    *count = static_cast<uint32>(associated.size());
    return Result::Success;
}

size_t Platform::GetScreenObjectSize() const
{
    return sizeof(Screen);
}

namespace
{
// The OS calls of 301770 and 54c570: Platform+0x2908 (D3DKMTQueryAdapterInfo), 2e99b38 (CRT wcstombs) and the
// kernel32 imports.
struct OglRuntimeLoaderOs
{
    NTSTATUS QueryAdapterInfo(OglAdapterQuery *query)
    {
        return D3DKMTQueryAdapterInfo(reinterpret_cast<D3DKMT_QUERYADAPTERINFO *>(query));
    }
    size_t WideToMultiByte(char *name, const wchar_t *wide, size_t count)
    {
        return wcstombs(name, wide, count);
    }
    bool ModuleHandle(const char *name, void **module)
    {
        return GetModuleHandleExA(0, name, reinterpret_cast<HMODULE *>(module)) != FALSE;
    }
    void *LoadModule(const char *name, uint32 flags)
    {
        return LoadLibraryExA(name, nullptr, flags);
    }
    void *GetProcAddress(void *module, const char *name)
    {
        return reinterpret_cast<void *>(::GetProcAddress(static_cast<HMODULE>(module), name));
    }
};
} // namespace

void Platform::LoadOglRuntime(Device &device)
{
    // 301770: the adapter object of the Device (Device+0xd9c0, hAdapter at +0x970).
    OglRuntimeLoaderOs os;
    Wddm::LoadOglRuntime(os, &m_oglRuntime, device.OsTransport().AdapterHandle());
}

// Native Platform slot 27 (2cce40). The escape goes through the master Device of the first adapter object, and a
// Device that supplies primaries must share that adapter object. Each Device here owns its adapter, so equal LUIDs
// stand in for the same adapter object.
Result Platform::TurboSyncControl(const TurboSyncControlInput& input)
{
    // The original dereferences the first adapter object before reading the input; with no Device it would fault.
    if ((m_deviceCount == 0) || (m_pDevice[0] == nullptr)) { return Result::ErrorUnavailable; }
    auto* primary = static_cast<Device*>(m_pDevice[0]);
    TurboSyncDeviceSlot devices[MaxDevices] = {};
    for (uint32 gpu = 0; gpu < MaxDevices; ++gpu)
    {
        auto* device = static_cast<Device*>(m_pDevice[gpu]);
        if (device == nullptr) { continue; }
        devices[gpu].present = true;
        devices[gpu].onPrimaryAdapter = (device->Adapter().luidLow == primary->Adapter().luidLow) &&
                                        (device->Adapter().luidHigh == primary->Adapter().luidHigh);
        devices[gpu].physicalAdapter = DeviceEscapeRoute(device->OsTransport(), this).physicalAdapter;
    }
    return Wddm::TurboSyncControl(DeviceEscapeRoute(primary->OsTransport(), this), input, devices);
}
} // namespace Pal::Wddm
