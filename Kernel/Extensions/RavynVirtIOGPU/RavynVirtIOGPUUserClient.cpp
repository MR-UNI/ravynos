#include "RavynVirtIOGPU.h"

#include <IOKit/IOLib.h>

#define super IOUserClient
OSDefineMetaClassAndStructors(RavynVirtIOGPUUserClient, IOUserClient);

bool
RavynVirtIOGPUUserClient::initWithTask(task_t owningTask,
                                       void *securityID,
                                       UInt32 type,
                                       OSDictionary *properties)
{
    fTask = owningTask;
    fGPU = NULL;
    fClientToken = 0;
    fResourceCount = 0;
    fFenceCount = 0;
    bzero(fResourceIds, sizeof(fResourceIds));
    bzero(fFenceIds, sizeof(fFenceIds));

    return super::initWithTask(owningTask, securityID, type, properties);
}

bool
RavynVirtIOGPUUserClient::start(IOService *provider)
{
    if (!super::start(provider)) {
        return false;
    }

    fGPU = OSDynamicCast(RavynVirtIOGPU, provider);
    if (!fGPU) {
        return false;
    }

    fClientToken = (uintptr_t)this;
    return true;
}

void
RavynVirtIOGPUUserClient::stop(IOService *provider)
{
    if (fGPU) {
        fGPU->cleanupClient(fClientToken);
    }

    super::stop(provider);
}

void
RavynVirtIOGPUUserClient::free(void)
{
    fGPU = NULL;
    fTask = TASK_NULL;
    fClientToken = 0;
    super::free();
}

IOReturn
RavynVirtIOGPUUserClient::clientClose(void)
{
    if (fGPU) {
        fGPU->cleanupClient(fClientToken);
    }

    if (!isInactive()) {
        terminate();
    }

    return kIOReturnSuccess;
}

IOReturn
RavynVirtIOGPUUserClient::externalMethod(uint32_t selector,
                                         IOExternalMethodArguments *arguments,
                                         IOExternalMethodDispatch *dispatch,
                                         OSObject *target,
                                         void *reference)
{
    static const IOExternalMethodDispatch methods[RavynVirtIOGPU::kMethodCount] = {
        { sSubmitCommandBuffer, 2, kIOUCVariableStructureSize, 1, 0 },
        { sCreateResource, 0, sizeof(RavynVirtIOGPU::ResourceCreateRequest), 1, 0 },
        { sMapResource, 1, 0, 2, 0 },
        { sWaitFence, 2, 0, 1, 0 },
    };

    if (!arguments || selector >= RavynVirtIOGPU::kMethodCount) {
        return kIOReturnBadArgument;
    }

    target = this;
    dispatch = const_cast<IOExternalMethodDispatch *>(&methods[selector]);
    reference = NULL;

    return super::externalMethod(selector, arguments, dispatch, target, reference);
}

IOReturn
RavynVirtIOGPUUserClient::clientMemoryForType(UInt32 type,
                                               IOOptionBits *options,
                                               IOMemoryDescriptor **memory)
{
    if (!options || !memory || !fGPU) {
        return kIOReturnBadArgument;
    }

    if (!ownsResource(type)) {
        return kIOReturnNotPermitted;
    }

    IOMemoryDescriptor *resource = fGPU->copyResourceMemoryForClient(fClientToken, type);
    if (!resource) {
        return kIOReturnNotFound;
    }

    *options = 0;
    *memory = resource;
    return kIOReturnSuccess;
}

IOReturn
RavynVirtIOGPUUserClient::sSubmitCommandBuffer(OSObject *target,
                                                void *,
                                                IOExternalMethodArguments *arguments)
{
    RavynVirtIOGPUUserClient *uc = OSDynamicCast(RavynVirtIOGPUUserClient, target);
    return uc ? uc->submitCommandBuffer(arguments) : kIOReturnBadArgument;
}

IOReturn
RavynVirtIOGPUUserClient::sCreateResource(OSObject *target,
                                          void *,
                                          IOExternalMethodArguments *arguments)
{
    RavynVirtIOGPUUserClient *uc = OSDynamicCast(RavynVirtIOGPUUserClient, target);
    return uc ? uc->createResource(arguments) : kIOReturnBadArgument;
}

IOReturn
RavynVirtIOGPUUserClient::sMapResource(OSObject *target,
                                       void *,
                                       IOExternalMethodArguments *arguments)
{
    RavynVirtIOGPUUserClient *uc = OSDynamicCast(RavynVirtIOGPUUserClient, target);
    return uc ? uc->mapResource(arguments) : kIOReturnBadArgument;
}

IOReturn
RavynVirtIOGPUUserClient::sWaitFence(OSObject *target,
                                     void *,
                                     IOExternalMethodArguments *arguments)
{
    RavynVirtIOGPUUserClient *uc = OSDynamicCast(RavynVirtIOGPUUserClient, target);
    return uc ? uc->waitFence(arguments) : kIOReturnBadArgument;
}

IOReturn
RavynVirtIOGPUUserClient::submitCommandBuffer(IOExternalMethodArguments *arguments)
{
    if (!arguments || !fGPU || arguments->scalarInputCount < 2 || arguments->scalarOutputCount < 1) {
        return kIOReturnBadArgument;
    }

    IOMemoryDescriptor *command = arguments->structureInputDescriptor;
    if (!command) {
        return kIOReturnNoMemory;
    }

    if (!command->getLength() || command->getLength() > RAVYN_VIRTIO_GPU_MAX_COMMAND_BYTES) {
        return kIOReturnMessageTooLarge;
    }

    const UInt32 resourceId = (UInt32)arguments->scalarInput[0];
    const UInt32 submitFlags = (UInt32)arguments->scalarInput[1];

    if (resourceId && !ownsResource(resourceId)) {
        return kIOReturnNotPermitted;
    }

    UInt64 fenceId = 0;
    IOReturn ret = fGPU->submitCommandBufferForClient(
        fClientToken,
        resourceId,
        submitFlags,
        command,
        &fenceId);
    if (ret != kIOReturnSuccess) {
        return ret;
    }

    if (!rememberFence(fenceId)) {
        return kIOReturnNoSpace;
    }

    arguments->scalarOutput[0] = fenceId;
    arguments->scalarOutputCount = 1;
    return kIOReturnSuccess;
}

IOReturn
RavynVirtIOGPUUserClient::createResource(IOExternalMethodArguments *arguments)
{
    if (!arguments || !fGPU || !arguments->structureInput ||
        arguments->structureInputSize != sizeof(RavynVirtIOGPU::ResourceCreateRequest) ||
        arguments->scalarOutputCount < 1) {
        return kIOReturnBadArgument;
    }

    RavynVirtIOGPU::ResourceCreateRequest req;
    bcopy(arguments->structureInput, &req, sizeof(req));

    UInt32 createdId = 0;
    IOReturn ret = fGPU->createResourceForClient(fClientToken, req, &createdId);
    if (ret != kIOReturnSuccess) {
        return ret;
    }

    if (!rememberResource(createdId)) {
        return kIOReturnNoSpace;
    }

    arguments->scalarOutput[0] = createdId;
    arguments->scalarOutputCount = 1;
    return kIOReturnSuccess;
}

IOReturn
RavynVirtIOGPUUserClient::mapResource(IOExternalMethodArguments *arguments)
{
    if (!arguments || !fGPU || arguments->scalarInputCount < 1 || arguments->scalarOutputCount < 2) {
        return kIOReturnBadArgument;
    }

    const UInt32 resourceId = (UInt32)arguments->scalarInput[0];
    if (!ownsResource(resourceId)) {
        return kIOReturnNotPermitted;
    }

    UInt32 memoryType = 0;
    UInt32 byteSize = 0;
    IOReturn ret = fGPU->mapResourceForClient(fClientToken, resourceId, &memoryType, &byteSize);
    if (ret != kIOReturnSuccess) {
        return ret;
    }

    arguments->scalarOutput[0] = memoryType;
    arguments->scalarOutput[1] = byteSize;
    arguments->scalarOutputCount = 2;
    return kIOReturnSuccess;
}

IOReturn
RavynVirtIOGPUUserClient::waitFence(IOExternalMethodArguments *arguments)
{
    if (!arguments || !fGPU || arguments->scalarInputCount < 2 || arguments->scalarOutputCount < 1) {
        return kIOReturnBadArgument;
    }

    const UInt64 fenceId = arguments->scalarInput[0];
    const UInt32 timeoutMs = (UInt32)arguments->scalarInput[1];

    if (!ownsFence(fenceId)) {
        return kIOReturnNotPermitted;
    }

    IOReturn ret = fGPU->waitFenceForClient(fClientToken, fenceId, timeoutMs);
    if (ret != kIOReturnSuccess) {
        return ret;
    }

    arguments->scalarOutput[0] = 1;
    arguments->scalarOutputCount = 1;
    return kIOReturnSuccess;
}

bool
RavynVirtIOGPUUserClient::rememberResource(UInt32 resourceId)
{
    if (fResourceCount >= kClientResourceTrackMax) {
        return false;
    }

    fResourceIds[fResourceCount++] = resourceId;
    return true;
}

bool
RavynVirtIOGPUUserClient::rememberFence(UInt64 fenceId)
{
    if (fFenceCount >= kClientFenceTrackMax) {
        return false;
    }

    fFenceIds[fFenceCount++] = fenceId;
    return true;
}

bool
RavynVirtIOGPUUserClient::ownsResource(UInt32 resourceId) const
{
    for (UInt32 i = 0; i < fResourceCount; i++) {
        if (fResourceIds[i] == resourceId) {
            return true;
        }
    }

    return false;
}

bool
RavynVirtIOGPUUserClient::ownsFence(UInt64 fenceId) const
{
    for (UInt32 i = 0; i < fFenceCount; i++) {
        if (fFenceIds[i] == fenceId) {
            return true;
        }
    }

    return false;
}
