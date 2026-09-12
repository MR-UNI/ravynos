#include "RavynVirtIOGPU.h"

#include <IOKit/IOLib.h>

#define super IOService
OSDefineMetaClassAndStructors(RavynVirtIOGPU, IOService);

IOService *
RavynVirtIOGPU::probe(IOService *provider, SInt32 *score)
{
    if (!OSDynamicCast(IOPCIDevice, provider)) {
        return NULL;
    }

    if (score) {
        *score += 1200;
    }
    return this;
}

bool
RavynVirtIOGPU::start(IOService *provider)
{
    if (!super::start(provider)) {
        return false;
    }

    fProvider = OSDynamicCast(IOPCIDevice, provider);
    if (!fProvider) {
        return false;
    }

    fStateLock = IOLockAlloc();
    if (!fStateLock) {
        return false;
    }

    bzero(fResources, sizeof(fResources));
    bzero(fFences, sizeof(fFences));

    fProvider->setMemoryEnable(true);
    fProvider->setBusMasterEnable(true);

    fScanoutWidth = RAVYN_VIRTIO_GPU_SCANOUT_WIDTH;
    fScanoutHeight = RAVYN_VIRTIO_GPU_SCANOUT_HEIGHT;
    fScanoutDepth = 32;
    fScanoutStride = fScanoutWidth * 4;
    fScanoutResourceId = 0;
    fNextFenceId = 1;

    IOByteCount scanoutBytes = (IOByteCount)(fScanoutStride * fScanoutHeight);
    fScanout = IOBufferMemoryDescriptor::inTaskWithOptions(
        kernel_task,
        kIOMemoryKernelUserShared | kIOMemoryPhysicallyContiguous,
        scanoutBytes,
        PAGE_SIZE);
    if (!fScanout) {
        return false;
    }
    bzero(fScanout->getBytesNoCopy(), scanoutBytes);

    IOReturn err = initTransport();
    if (err != kIOReturnSuccess) {
        return false;
    }

    registerService();
    return true;
}

void
RavynVirtIOGPU::stop(IOService *provider)
{
    if (fStateLock) {
        IOLockLock(fStateLock);
        fDeviceOnline = false;
        IOLockUnlock(fStateLock);
    }

    resetTransport();

    OSSafeReleaseNULL(fScanout);
    fProvider = NULL;

    super::stop(provider);
}

void
RavynVirtIOGPU::free(void)
{
    resetTransport();
    OSSafeReleaseNULL(fScanout);

    if (fStateLock) {
        IOLockFree(fStateLock);
        fStateLock = NULL;
    }

    super::free();
}

IOReturn
RavynVirtIOGPU::newUserClient(task_t owningTask,
                               void *securityID,
                               UInt32 type,
                               OSDictionary *properties,
                               IOUserClient **handler)
{
    if (!handler) {
        return kIOReturnBadArgument;
    }

    *handler = NULL;

    RavynVirtIOGPUUserClient *client = OSTypeAlloc(RavynVirtIOGPUUserClient);
    if (!client) {
        return kIOReturnNoMemory;
    }

    if (!client->initWithTask(owningTask, securityID, type, properties)) {
        client->release();
        return kIOReturnNoMemory;
    }

    if (!client->attach(this)) {
        client->release();
        return kIOReturnNotAttached;
    }

    if (!client->start(this)) {
        client->detach(this);
        client->release();
        return kIOReturnNotAttached;
    }

    *handler = client;
    return kIOReturnSuccess;
}

IOReturn
RavynVirtIOGPU::createResourceForClient(uintptr_t owner,
                                        const ResourceCreateRequest &request,
                                        UInt32 *createdId)
{
    if (!request.resourceId || !request.byteSize || request.byteSize > RAVYN_VIRTIO_GPU_MAX_COMMAND_BYTES) {
        return kIOReturnBadArgument;
    }

    if (!request.width || !request.height || !request.stride) {
        return kIOReturnBadArgument;
    }

    if ((UInt64)request.stride * (UInt64)request.height > (UInt64)request.byteSize) {
        return kIOReturnBadArgument;
    }

    IOBufferMemoryDescriptor *backing = IOBufferMemoryDescriptor::inTaskWithOptions(
        kernel_task,
        kIOMemoryKernelUserShared | kIOMemoryPhysicallyContiguous,
        request.byteSize,
        PAGE_SIZE);
    if (!backing) {
        return kIOReturnNoMemory;
    }

    IOReturn ret = kIOReturnNoResources;

    IOLockLock(fStateLock);

    if (!fDeviceOnline) {
        ret = kIOReturnOffline;
        goto out;
    }

    if (findResourceLocked(request.resourceId)) {
        ret = kIOReturnExclusiveAccess;
        goto out;
    }

    ResourceEntry *slot = allocResourceLocked();
    if (!slot) {
        ret = kIOReturnNoResources;
        goto out;
    }

    slot->inUse = true;
    slot->resourceId = request.resourceId;
    slot->width = request.width;
    slot->height = request.height;
    slot->stride = request.stride;
    slot->format = request.format;
    slot->bindFlags = request.bindFlags;
    slot->byteSize = request.byteSize;
    slot->lastFence = 0;
    slot->owner = owner;
    slot->backing = backing;
    backing = NULL;

    if (createdId) {
        *createdId = slot->resourceId;
    }

    ret = kIOReturnSuccess;

out:
    IOLockUnlock(fStateLock);
    OSSafeReleaseNULL(backing);
    return ret;
}

IOReturn
RavynVirtIOGPU::mapResourceForClient(uintptr_t owner,
                                     UInt32 resourceId,
                                     UInt32 *memoryType,
                                     UInt32 *byteSize)
{
    if (!resourceId || !memoryType || !byteSize) {
        return kIOReturnBadArgument;
    }

    IOLockLock(fStateLock);

    if (!fDeviceOnline) {
        IOLockUnlock(fStateLock);
        return kIOReturnOffline;
    }

    ResourceEntry *resource = findResourceLocked(resourceId);
    if (!resource || resource->owner != owner || !resource->backing) {
        IOLockUnlock(fStateLock);
        return kIOReturnNotPermitted;
    }

    *memoryType = resource->resourceId;
    *byteSize = resource->byteSize;

    IOLockUnlock(fStateLock);
    return kIOReturnSuccess;
}

IOMemoryDescriptor *
RavynVirtIOGPU::copyResourceMemoryForClient(uintptr_t owner,
                                            UInt32 resourceId)
{
    IOMemoryDescriptor *mem = NULL;

    IOLockLock(fStateLock);

    ResourceEntry *resource = findResourceLocked(resourceId);
    if (resource && resource->owner == owner && resource->backing) {
        resource->backing->retain();
        mem = resource->backing;
    }

    IOLockUnlock(fStateLock);
    return mem;
}

IOReturn
RavynVirtIOGPU::submitCommandBufferForClient(uintptr_t owner,
                                              UInt32 resourceId,
                                              UInt32 submitFlags,
                                              IOMemoryDescriptor *command,
                                              UInt64 *fenceId)
{
    if (!command || !fenceId) {
        return kIOReturnBadArgument;
    }

    const UInt64 cmdLen = command->getLength();
    if (!cmdLen || cmdLen > RAVYN_VIRTIO_GPU_MAX_COMMAND_BYTES) {
        return kIOReturnMessageTooLarge;
    }

    IOReturn ret = kIOReturnSuccess;

    IOLockLock(fStateLock);

    if (!fDeviceOnline) {
        ret = kIOReturnOffline;
        goto out;
    }

    ResourceEntry *resource = NULL;
    if (resourceId) {
        resource = findResourceLocked(resourceId);
        if (!resource || resource->owner != owner) {
            ret = kIOReturnNotPermitted;
            goto out;
        }
    }

    FenceEntry *fence = allocFenceLocked();
    if (!fence) {
        ret = kIOReturnNoResources;
        goto out;
    }

    fence->inUse = true;
    fence->signaled = false;
    fence->owner = owner;
    fence->fenceId = fNextFenceId++;

    if (!fNextFenceId) {
        fNextFenceId = 1;
    }

    if (resource) {
        resource->lastFence = fence->fenceId;
        if (submitFlags & kSubmitFlagPromoteToScanout) {
            fScanoutResourceId = resource->resourceId;
            if (resource->backing && fScanout) {
                const IOByteCount copyBytes =
                    (resource->byteSize < fScanout->getLength()) ?
                    resource->byteSize : fScanout->getLength();
                bcopy(resource->backing->getBytesNoCopy(), fScanout->getBytesNoCopy(), copyBytes);
            }
        }
    }

    /*
     * Thin transport placeholder:
     * command payload accepted and fence issued; real virtqueue execution
     * and host completion wiring can replace this signaling point.
     */
    signalFenceLocked(fence->fenceId);

    *fenceId = fence->fenceId;

out:
    IOLockUnlock(fStateLock);
    return ret;
}

IOReturn
RavynVirtIOGPU::waitFenceForClient(uintptr_t owner,
                                   UInt64 fenceId,
                                   UInt32 timeoutMs)
{
    UInt32 waitedMs = 0;

    while (true) {
        IOLockLock(fStateLock);

        if (!fDeviceOnline) {
            IOLockUnlock(fStateLock);
            return kIOReturnOffline;
        }

        FenceEntry *fence = findFenceLocked(fenceId);
        if (!fence || fence->owner != owner) {
            IOLockUnlock(fStateLock);
            return kIOReturnNotPermitted;
        }

        if (fence->signaled) {
            IOLockUnlock(fStateLock);
            return kIOReturnSuccess;
        }

        IOLockUnlock(fStateLock);

        if (waitedMs >= timeoutMs) {
            return kIOReturnTimeout;
        }

        IOSleep(1);
        waitedMs++;
    }
}

void
RavynVirtIOGPU::cleanupClient(uintptr_t owner)
{
    IOLockLock(fStateLock);

    for (UInt32 i = 0; i < RAVYN_VIRTIO_GPU_MAX_RESOURCES; i++) {
        if (!fResources[i].inUse || fResources[i].owner != owner) {
            continue;
        }

        if (fScanoutResourceId == fResources[i].resourceId) {
            fScanoutResourceId = 0;
            if (fScanout) {
                bzero(fScanout->getBytesNoCopy(), fScanout->getLength());
            }
        }

        freeResourceLocked(&fResources[i]);
    }

    for (UInt32 i = 0; i < RAVYN_VIRTIO_GPU_MAX_FENCES; i++) {
        if (!fFences[i].inUse || fFences[i].owner != owner) {
            continue;
        }
        bzero(&fFences[i], sizeof(fFences[i]));
    }

    IOLockUnlock(fStateLock);
}

IOReturn
RavynVirtIOGPU::getScanoutConfig(UInt32 *width,
                                 UInt32 *height,
                                 UInt32 *stride,
                                 UInt32 *depth) const
{
    if (!width || !height || !stride || !depth) {
        return kIOReturnBadArgument;
    }

    *width = fScanoutWidth;
    *height = fScanoutHeight;
    *stride = fScanoutStride;
    *depth = fScanoutDepth;
    return kIOReturnSuccess;
}

IOBufferMemoryDescriptor *
RavynVirtIOGPU::copyScanoutMemory(void)
{
    if (!fScanout) {
        return NULL;
    }

    fScanout->retain();
    return fScanout;
}

IOReturn
RavynVirtIOGPU::initTransport(void)
{
    IOLockLock(fStateLock);
    fDeviceOnline = true;
    IOLockUnlock(fStateLock);

    return kIOReturnSuccess;
}

void
RavynVirtIOGPU::resetTransport(void)
{
    if (!fStateLock) {
        return;
    }

    IOLockLock(fStateLock);
    fDeviceOnline = false;

    for (UInt32 i = 0; i < RAVYN_VIRTIO_GPU_MAX_RESOURCES; i++) {
        if (!fResources[i].inUse) {
            continue;
        }
        freeResourceLocked(&fResources[i]);
    }

    bzero(fFences, sizeof(fFences));
    fScanoutResourceId = 0;

    IOLockUnlock(fStateLock);
}

RavynVirtIOGPU::ResourceEntry *
RavynVirtIOGPU::findResourceLocked(UInt32 resourceId)
{
    for (UInt32 i = 0; i < RAVYN_VIRTIO_GPU_MAX_RESOURCES; i++) {
        if (fResources[i].inUse && fResources[i].resourceId == resourceId) {
            return &fResources[i];
        }
    }
    return NULL;
}

RavynVirtIOGPU::ResourceEntry *
RavynVirtIOGPU::allocResourceLocked(void)
{
    for (UInt32 i = 0; i < RAVYN_VIRTIO_GPU_MAX_RESOURCES; i++) {
        if (!fResources[i].inUse) {
            return &fResources[i];
        }
    }
    return NULL;
}

void
RavynVirtIOGPU::freeResourceLocked(ResourceEntry *resource)
{
    if (!resource) {
        return;
    }

    OSSafeReleaseNULL(resource->backing);
    bzero(resource, sizeof(*resource));
}

RavynVirtIOGPU::FenceEntry *
RavynVirtIOGPU::findFenceLocked(UInt64 fenceId)
{
    for (UInt32 i = 0; i < RAVYN_VIRTIO_GPU_MAX_FENCES; i++) {
        if (fFences[i].inUse && fFences[i].fenceId == fenceId) {
            return &fFences[i];
        }
    }
    return NULL;
}

RavynVirtIOGPU::FenceEntry *
RavynVirtIOGPU::allocFenceLocked(void)
{
    for (UInt32 i = 0; i < RAVYN_VIRTIO_GPU_MAX_FENCES; i++) {
        if (!fFences[i].inUse) {
            return &fFences[i];
        }
    }
    return NULL;
}

void
RavynVirtIOGPU::signalFenceLocked(UInt64 fenceId)
{
    FenceEntry *fence = findFenceLocked(fenceId);
    if (fence) {
        fence->signaled = true;
    }
}
