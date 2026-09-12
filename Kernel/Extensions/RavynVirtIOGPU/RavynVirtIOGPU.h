/*
 * RavynVirtIOGPU: minimal VirtIO GPU transport + user client + framebuffer glue.
 *
 * Copyright (C) 2026 ravynOS Project. All rights reserved.
 */

#ifndef _RAVYN_VIRTIO_GPU_H
#define _RAVYN_VIRTIO_GPU_H

#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOLocks.h>
#include <IOKit/graphics/IOFramebuffer.h>
#include <IOKit/pci/IOPCIDevice.h>

#define RAVYN_VIRTIO_GPU_MAX_RESOURCES 256
#define RAVYN_VIRTIO_GPU_MAX_FENCES 512
#define RAVYN_VIRTIO_GPU_MAX_COMMAND_BYTES (4 * 1024 * 1024)
#define RAVYN_VIRTIO_GPU_SCANOUT_WIDTH 1024
#define RAVYN_VIRTIO_GPU_SCANOUT_HEIGHT 768

class RavynVirtIOGPU : public IOService
{
    OSDeclareDefaultStructors(RavynVirtIOGPU);

public:
    enum : UInt32 {
        kSubmitCommandBufferSelector = 0,
        kCreateResourceSelector = 1,
        kMapResourceSelector = 2,
        kWaitFenceSelector = 3,
        kMethodCount = 4,
    };

    struct ResourceCreateRequest {
        UInt32 resourceId;
        UInt32 width;
        UInt32 height;
        UInt32 stride;
        UInt32 format;
        UInt32 bindFlags;
        UInt32 byteSize;
        UInt32 reserved;
    };

    enum : UInt32 {
        kSubmitFlagPromoteToScanout = 1u << 0,
    };

    IOService *probe(IOService *provider, SInt32 *score) APPLE_KEXT_OVERRIDE;
    bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
    void stop(IOService *provider) APPLE_KEXT_OVERRIDE;
    void free(void) APPLE_KEXT_OVERRIDE;

    IOReturn newUserClient(task_t owningTask,
                           void *securityID,
                           UInt32 type,
                           OSDictionary *properties,
                           IOUserClient **handler) APPLE_KEXT_OVERRIDE;

    IOReturn createResourceForClient(uintptr_t owner,
                                     const ResourceCreateRequest &request,
                                     UInt32 *createdId);
    IOReturn mapResourceForClient(uintptr_t owner,
                                  UInt32 resourceId,
                                  UInt32 *memoryType,
                                  UInt32 *byteSize);
    IOMemoryDescriptor *copyResourceMemoryForClient(uintptr_t owner,
                                                    UInt32 resourceId);
    IOReturn submitCommandBufferForClient(uintptr_t owner,
                                          UInt32 resourceId,
                                          UInt32 submitFlags,
                                          IOMemoryDescriptor *command,
                                          UInt64 *fenceId);
    IOReturn waitFenceForClient(uintptr_t owner,
                                UInt64 fenceId,
                                UInt32 timeoutMs);
    void cleanupClient(uintptr_t owner);

    IOReturn getScanoutConfig(UInt32 *width,
                              UInt32 *height,
                              UInt32 *stride,
                              UInt32 *depth) const;
    IOBufferMemoryDescriptor *copyScanoutMemory(void);

private:
    struct ResourceEntry {
        bool                       inUse;
        UInt32                     resourceId;
        UInt32                     width;
        UInt32                     height;
        UInt32                     stride;
        UInt32                     format;
        UInt32                     bindFlags;
        UInt32                     byteSize;
        UInt64                     lastFence;
        uintptr_t                  owner;
        IOBufferMemoryDescriptor * backing;
    };

    struct FenceEntry {
        bool      inUse;
        bool      signaled;
        UInt64    fenceId;
        uintptr_t owner;
    };

    IOPCIDevice *fProvider;
    IOLock      *fStateLock;
    volatile bool fDeviceOnline;

    ResourceEntry fResources[RAVYN_VIRTIO_GPU_MAX_RESOURCES];
    FenceEntry    fFences[RAVYN_VIRTIO_GPU_MAX_FENCES];

    IOBufferMemoryDescriptor *fScanout;
    UInt32 fScanoutWidth;
    UInt32 fScanoutHeight;
    UInt32 fScanoutStride;
    UInt32 fScanoutDepth;
    UInt32 fScanoutResourceId;

    UInt64 fNextFenceId;

    IOReturn initTransport(void);
    void resetTransport(void);

    ResourceEntry *findResourceLocked(UInt32 resourceId);
    ResourceEntry *allocResourceLocked(void);
    void freeResourceLocked(ResourceEntry *resource);

    FenceEntry *findFenceLocked(UInt64 fenceId);
    FenceEntry *allocFenceLocked(void);
    void signalFenceLocked(UInt64 fenceId);
};

class RavynVirtIOGPUUserClient : public IOUserClient
{
    OSDeclareDefaultStructors(RavynVirtIOGPUUserClient);

public:
    bool initWithTask(task_t owningTask,
                      void *securityID,
                      UInt32 type,
                      OSDictionary *properties) APPLE_KEXT_OVERRIDE;
    bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
    void stop(IOService *provider) APPLE_KEXT_OVERRIDE;
    void free(void) APPLE_KEXT_OVERRIDE;

    IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
    IOReturn externalMethod(uint32_t selector,
                            IOExternalMethodArguments *arguments,
                            IOExternalMethodDispatch *dispatch,
                            OSObject *target,
                            void *reference) APPLE_KEXT_OVERRIDE;
    IOReturn clientMemoryForType(UInt32 type,
                                 IOOptionBits *options,
                                 IOMemoryDescriptor **memory) APPLE_KEXT_OVERRIDE;

private:
    enum : UInt32 {
        kClientResourceTrackMax = 256,
        kClientFenceTrackMax = 512,
    };

    RavynVirtIOGPU *fGPU;
    task_t          fTask;
    uintptr_t       fClientToken;

    UInt32 fResourceIds[kClientResourceTrackMax];
    UInt32 fResourceCount;

    UInt64 fFenceIds[kClientFenceTrackMax];
    UInt32 fFenceCount;

    static IOReturn sSubmitCommandBuffer(OSObject *target,
                                         void *reference,
                                         IOExternalMethodArguments *arguments);
    static IOReturn sCreateResource(OSObject *target,
                                    void *reference,
                                    IOExternalMethodArguments *arguments);
    static IOReturn sMapResource(OSObject *target,
                                 void *reference,
                                 IOExternalMethodArguments *arguments);
    static IOReturn sWaitFence(OSObject *target,
                               void *reference,
                               IOExternalMethodArguments *arguments);

    IOReturn submitCommandBuffer(IOExternalMethodArguments *arguments);
    IOReturn createResource(IOExternalMethodArguments *arguments);
    IOReturn mapResource(IOExternalMethodArguments *arguments);
    IOReturn waitFence(IOExternalMethodArguments *arguments);

    bool rememberResource(UInt32 resourceId);
    bool rememberFence(UInt64 fenceId);
    bool ownsResource(UInt32 resourceId) const;
    bool ownsFence(UInt64 fenceId) const;
};

class RavynVirtIOGPUFramebuffer : public IOFramebuffer
{
    OSDeclareDefaultStructors(RavynVirtIOGPUFramebuffer);

public:
    IOService *probe(IOService *provider, SInt32 *score) APPLE_KEXT_OVERRIDE;
    bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
    void stop(IOService *provider) APPLE_KEXT_OVERRIDE;

    IOReturn enableController(void) APPLE_KEXT_OVERRIDE;
    const char *getPixelFormats(void) APPLE_KEXT_OVERRIDE;
    IOReturn getCurrentDisplayMode(IODisplayModeID *displayMode,
                                   IOIndex *depth) APPLE_KEXT_OVERRIDE;
    IOReturn setDisplayMode(IODisplayModeID displayMode,
                            IOIndex depth) APPLE_KEXT_OVERRIDE;
    IODeviceMemory *getApertureRange(IOPixelAperture aperture) APPLE_KEXT_OVERRIDE;
    IOReturn getInformationForDisplayMode(IODisplayModeID displayMode,
                                          IODisplayModeInformation *info) APPLE_KEXT_OVERRIDE;
    UInt64 getPixelFormatsForDisplayMode(IODisplayModeID displayMode,
                                         IOIndex depth) APPLE_KEXT_OVERRIDE;
    IOReturn getPixelInformation(IODisplayModeID displayMode,
                                 IOIndex depth,
                                 IOPixelAperture aperture,
                                 IOPixelInformation *info) APPLE_KEXT_OVERRIDE;
    IOReturn getDisplayModes(IODisplayModeID *allDisplayModes) APPLE_KEXT_OVERRIDE;
    IOItemCount getDisplayModeCount(void) APPLE_KEXT_OVERRIDE;
    bool isConsoleDevice(void) APPLE_KEXT_OVERRIDE;

private:
    enum : IODisplayModeID {
        kVirtIOGPUDisplayMode = 1,
    };

    RavynVirtIOGPU            *fGPU;
    IOBufferMemoryDescriptor  *fScanout;
    UInt32                     fWidth;
    UInt32                     fHeight;
    UInt32                     fStride;
    UInt32                     fDepth;
};

#endif /* _RAVYN_VIRTIO_GPU_H */
