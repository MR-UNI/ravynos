#include "RavynVirtIOGPU.h"

#include <IOKit/IOLib.h>

#define super IOFramebuffer
OSDefineMetaClassAndStructors(RavynVirtIOGPUFramebuffer, IOFramebuffer);

IOService *
RavynVirtIOGPUFramebuffer::probe(IOService *provider, SInt32 *score)
{
    if (!OSDynamicCast(RavynVirtIOGPU, provider)) {
        return NULL;
    }

    if (score) {
        *score += 1100;
    }

    return this;
}

bool
RavynVirtIOGPUFramebuffer::start(IOService *provider)
{
    if (!super::start(provider)) {
        return false;
    }

    fGPU = OSDynamicCast(RavynVirtIOGPU, provider);
    if (!fGPU) {
        return false;
    }

    if (fGPU->getScanoutConfig(&fWidth, &fHeight, &fStride, &fDepth) != kIOReturnSuccess) {
        return false;
    }

    fScanout = fGPU->copyScanoutMemory();
    if (!fScanout) {
        return false;
    }

    setupForCurrentConfig();
    registerService();
    return true;
}

void
RavynVirtIOGPUFramebuffer::stop(IOService *provider)
{
    OSSafeReleaseNULL(fScanout);
    fGPU = NULL;
    super::stop(provider);
}

IOReturn
RavynVirtIOGPUFramebuffer::enableController(void)
{
    return kIOReturnSuccess;
}

const char *
RavynVirtIOGPUFramebuffer::getPixelFormats(void)
{
    return IO32BitDirectPixels;
}

IOReturn
RavynVirtIOGPUFramebuffer::getCurrentDisplayMode(IODisplayModeID *displayMode,
                                                 IOIndex *depth)
{
    if (!displayMode || !depth) {
        return kIOReturnBadArgument;
    }

    *displayMode = kVirtIOGPUDisplayMode;
    *depth = 32;
    return kIOReturnSuccess;
}

IOReturn
RavynVirtIOGPUFramebuffer::setDisplayMode(IODisplayModeID displayMode,
                                          IOIndex depth)
{
    if (displayMode != kVirtIOGPUDisplayMode || depth != 32) {
        return kIOReturnUnsupportedMode;
    }

    return kIOReturnSuccess;
}

IODeviceMemory *
RavynVirtIOGPUFramebuffer::getApertureRange(IOPixelAperture)
{
    if (!fScanout) {
        return NULL;
    }

    return IODeviceMemory::withRange(
        (mach_vm_address_t)fScanout->getPhysicalAddress(),
        fScanout->getLength());
}

IOReturn
RavynVirtIOGPUFramebuffer::getInformationForDisplayMode(IODisplayModeID displayMode,
                                                         IODisplayModeInformation *info)
{
    if (displayMode != kVirtIOGPUDisplayMode || !info) {
        return kIOReturnBadArgument;
    }

    bzero(info, sizeof(*info));
    info->nominalWidth = fWidth;
    info->nominalHeight = fHeight;
    info->refreshRate = 60 << 16;
    info->maxDepthIndex = 32;
    return kIOReturnSuccess;
}

UInt64
RavynVirtIOGPUFramebuffer::getPixelFormatsForDisplayMode(IODisplayModeID displayMode,
                                                          IOIndex depth)
{
    if (displayMode != kVirtIOGPUDisplayMode || depth != 32) {
        return 0;
    }

    return (UInt64)(uintptr_t)getPixelFormats();
}

IOReturn
RavynVirtIOGPUFramebuffer::getPixelInformation(IODisplayModeID displayMode,
                                                IOIndex depth,
                                                IOPixelAperture aperture,
                                                IOPixelInformation *info)
{
    if (aperture || displayMode != kVirtIOGPUDisplayMode || depth != 32 || !info) {
        return kIOReturnUnsupportedMode;
    }

    bzero(info, sizeof(*info));
    info->activeWidth = fWidth;
    info->activeHeight = fHeight;
    info->bytesPerRow = fStride;
    strlcpy(info->pixelFormat, IO32BitDirectPixels, sizeof(info->pixelFormat));
    info->pixelType = kIORGBDirectPixels;
    info->componentMasks[0] = 0x00ff0000;
    info->componentMasks[1] = 0x0000ff00;
    info->componentMasks[2] = 0x000000ff;
    info->bitsPerPixel = 32;
    info->componentCount = 3;
    info->bitsPerComponent = 8;
    return kIOReturnSuccess;
}

IOReturn
RavynVirtIOGPUFramebuffer::getDisplayModes(IODisplayModeID *allDisplayModes)
{
    if (!allDisplayModes) {
        return kIOReturnBadArgument;
    }

    *allDisplayModes = kVirtIOGPUDisplayMode;
    return kIOReturnSuccess;
}

IOItemCount
RavynVirtIOGPUFramebuffer::getDisplayModeCount(void)
{
    return 1;
}

bool
RavynVirtIOGPUFramebuffer::isConsoleDevice(void)
{
    return false;
}
