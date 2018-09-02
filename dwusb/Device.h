/*++

Module Name:

    device.h

Abstract:

    This file contains the device definitions.

Environment:

    Kernel-mode Driver Framework

--*/

#include "public.h"
#include "dwc_otg_regs.h"

#include <usb.h>
#include <usbspec.h>
#include <Wdfusb.h>
#include <ucx/1.4/ucxclass.h>

EXTERN_C_START

//
// The device context performs the same job as
// a WDM device extension in the driver frameworks
//
typedef struct _DEVICE_CONTEXT
{
	WDFDEVICE Device;
	UCXCONTROLLER Controller;
	PHYSICAL_ADDRESS MemoryBase;
	WDFINTERRUPT Interrupt;


} DEVICE_CONTEXT, *PDEVICE_CONTEXT;

//
// This macro will generate an inline function called DeviceGetContext
// which will be used to get a pointer to the device context memory
// in a type safe manner.
//
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DEVICE_CONTEXT, DeviceGetContext);

#define USB_MAX_ADDRESS_COUNT 127

typedef struct _USB_ADDRESS_LIST {
	RTL_BITMAP Bitmap;
	ULONG Bits[4];
} USB_ADDRESS_LIST, *PUSB_ADDRESS_LIST;

typedef struct _CONTROLLER_DATA {
	dwc_otg_core_global_regs_t* CoreGlobalRegs;
	dwc_otg_host_global_regs_t* HostGlobalRegs;

	hwcfg1_data_t hwcfg1;
	hwcfg2_data_t hwcfg2;
	hwcfg3_data_t hwcfg3;
	hwcfg4_data_t hwcfg4;
	fifosize_data_t hptxfsiz;

	volatile uint32_t* PcgcCtl;

	WDFDEVICE WdfDevice;
	WDFINTERRUPT WdfInterrupt;

	PFN_CHANNEL_CALLBACK ChannelCallbacks[NUM_CHANNELS];
	PVOID ChannelCallbackContext[NUM_CHANNELS];

	PVOID CommonBufferBase[NUM_CHANNELS];
	PHYSICAL_ADDRESS CommonBufferBaseLA[NUM_CHANNELS];

	BOOLEAN UsbAddressInit;
	USB_ADDRESS_LIST UsbAddressList;

	BOOLEAN SmDpcInited;
	KDPC SmDpc;

	INT ChTtHubs[NUM_CHANNELS];
	INT ChTtPorts[NUM_CHANNELS];

	PVOID ChTrDatas[NUM_CHANNELS];

	UCXROOTHUB RootHub;

	volatile long ChannelMask;
	volatile long DeadChannelMask;
	WDFSPINLOCK ChannelMaskLock;
} CONTROLLER_DATA, *PCONTROLLER_DATA;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(CONTROLLER_DATA, ControllerGetData);

//
// Function to initialize the device and its callbacks
//
NTSTATUS
dwusbCreateDevice(
    _Inout_ PWDFDEVICE_INIT DeviceInit
    );

EXTERN_C_END
