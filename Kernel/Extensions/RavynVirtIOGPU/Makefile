MK_WERROR = no
SUBDIR =
KEXT = RavynVirtIOGPU
KERNEL_PRIVATE = yes
INFO_PLIST = Info.plist
BUNDLE_IDENTIFIER = com.ravynos.driver.RavynVirtIOGPU
BUNDLE_VERSION = 1.0.0

SRCS = RavynVirtIOGPU.cpp RavynVirtIOGPUUserClient.cpp RavynVirtIOGPUFramebuffer.cpp

CFLAGS = -Wno-implicit-int-conversion -Wno-sign-conversion \
	-I${ROOT_SOURCE_DIR}/Kernel/xnu/iokit \
	-I${ROOT_SOURCE_DIR}/Kernel/xnu/osfmk \
	-I${ROOT_SOURCE_DIR}/Kernel/xnu/bsd \
	-I${ROOT_SOURCE_DIR}/Kernel/xnu/libkern \
	-I${.CURDIR:H}/IOPCIFamily \
	-I${.CURDIR:H}/IOGraphics/IOGraphicsFamily \
	-I${ROOT_BINARY_DIR}${ROOT_SOURCE_DIR}/Kernel/xnu/EXPORT_HDRS/osfmk

.include <rvn.kext.mk>
