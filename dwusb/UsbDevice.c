/*++

Module Name:

device.c - Device handling events for example driver.

Abstract:

This file contains the device entry points and callbacks.

Environment:

Kernel-mode Driver Framework

--*/

#include "driver.h"
#include "UsbDevice.tmh"

// FIXME don't hardcode these! use resources!
//#define DWUSB_BASE 0x3F980000
//#define DWUSB_INT 0x29

NTSTATUS
UsbDevice_UcxEvtDeviceAdd(
	UCXCONTROLLER       UcxController,
	PUCXUSBDEVICE_INFO  UsbDeviceInfo,
	PUCXUSBDEVICE_INIT  UsbDeviceInit
);

#ifdef ALLOC_PRAGMA
#pragma alloc_text (PAGE, UsbDevice_UcxEvtDeviceAdd)
#endif

typedef struct _USBDEVICE_DATA
{
	UCXUSBDEVICE UcxUsbDevice;
	UCXCONTROLLER UcxController;

	UCXENDPOINT DefaultEndpoint;

	dwc_otg_host_global_regs_t* HostGlobalRegs;
	dwc_otg_hc_regs_t* ChannelRegs[NUM_CHANNELS];

	ULONG Address;
	UCXUSBDEVICE_INFO UsbDeviceInfo;
} USBDEVICE_DATA, *PUSBDEVICE_DATA;

typedef enum _ENDPOINT_DIRECTION
{
	Direction_In,
	Direction_Out,
	Direction_None
} ENDPOINT_DIRECTION;

typedef enum _ENDPOINT_TYPE
{
	EndpointType_Isoch,
	EndpointType_Bulk,
	EndpointType_Interrupt,
	EndpointType_Control
} ENDPOINT_TYPE;

typedef struct _ENDPOINT_DATA
{
	UCXUSBDEVICE UcxUsbDevice;
	PUSBDEVICE_DATA UsbDeviceHandle;

	USB_ENDPOINT_DESCRIPTOR UsbEndpointDescriptor;

	ENDPOINT_DIRECTION Direction;
	ENDPOINT_TYPE Type;

	ULONG MaxPacketSize;

	WDFQUEUE IoQueue;

	UINT8 InToggle;
	UINT8 OutToggle;

	BOOLEAN EpSmDpcInited;
	KDPC EpSmDpc;

	PEX_TIMER EpResumeTimer;
	PVOID EpResumeContext;
} ENDPOINT_DATA, *PENDPOINT_DATA;

typedef enum _CHSM_STATE
{
	CHSM_Idle,
	CHSM_ControlSetup,
	CHSM_ControlSetupWait,
	CHSM_ControlSetupDone,
	CHSM_ControlData,
	CHSM_ControlDataWait,
	CHSM_ControlDataDone,
	CHSM_ControlStatus,
	CHSM_ControlStatusWait,
	CHSM_ControlStatusDone,
	CHSM_AddressSetup,
	CHSM_AddressSetupWait,
	CHSM_AddressSetupDone,
	CHSM_AddressStatus,
	CHSM_AddressStatusWait,
	CHSM_AddressStatusDone,
	CHSM_InterruptOrBulkData,
	CHSM_InterruptOrBulkDataWait,
	CHSM_InterruptOrBulkDataDone,
} CHSM_STATE;

typedef struct _CHSM_DATA
{
	CHSM_STATE State;

	PURB Urb;
	WDFREQUEST Request;

	USHORT Address;
	INT Channel;
} CHSM_DATA, *PCHSM_DATA;

typedef enum _TRSM_STATE
{
	TRSM_Init,
	TRSM_CheckFreePort,
	TRSM_Transferring,
	TRSM_TransferWaiting,
	TRSM_TransferHalted,
	TRSM_Done
} TRSM_STATE;

typedef struct _TRSM_DATA
{
	TRSM_STATE State;

	UINT8 Pid;
	PVOID Buffer;
	ULONG Length;

	INT In;

	BOOLEAN DoSplit;
	BOOLEAN CompleteSplit;
	BOOLEAN IsRetry;
	INT Strikes;
	ULONG Done;

	ULONG XferLen;
	ULONG MaxXferLen;
	ULONG NumPackets;

	ULONG SSplitFrameNum;

	INT Channel;

	INT TtHub;
	INT TtPort;
} TRSM_DATA, *PTRSM_DATA;

typedef struct _TR_DATA
{
	PENDPOINT_DATA EndpointHandle;

	CHSM_DATA StateMachine;
	TRSM_DATA TrStateMachine;

	CHSM_DATA NextStateMachine;

	WDFSPINLOCK SpinLock;

	UINT8 StatusBuffer[64];
} TR_DATA, *PTR_DATA;

#define DWC_OTG_HOST_CHAN_REGS_OFFSET 0x500
#define DWC_OTG_CHAN_REGS_OFFSET 0x20

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(ENDPOINT_DATA, GetEndpointData)
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(USBDEVICE_DATA, GetUsbDeviceData)
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(TR_DATA, GetTRData)

VOID
Controller_InvokeTrSm(
	_In_ UCXCONTROLLER UcxController,
	_In_ PTR_DATA TrData
);

NTSTATUS
Controller_AllocateChannel(
	_In_ UCXCONTROLLER UcxController,
	_Out_ INT* Channel
)
{
	PCONTROLLER_DATA data = ControllerGetData(UcxController);
	NTSTATUS status = STATUS_INSUFFICIENT_RESOURCES;

	for (int i = 0; i < NUM_CHANNELS; i++)
	{
		WdfSpinLockAcquire(data->ChannelMaskLock);
		if (!(data->ChannelMask & (1 << i)))
		{
			InterlockedOr(&data->ChannelMask, 1 << i);

			*Channel = i;
			status = STATUS_SUCCESS;
		}
		WdfSpinLockRelease(data->ChannelMaskLock);

		if (NT_SUCCESS(status)) {
			break;
		}
	}

	if (!NT_SUCCESS(status)) {
		KdPrint((__FUNCTION__ ": Not enough channels\n"));
	}
	else
	{
		KdPrint((__FUNCTION__ ": Assigned channel %d\n", *Channel));
	}

	return status;
}

VOID 
Controller_ReleaseChannel(
	_In_ UCXCONTROLLER UcxController,
	_In_ INT Channel
)
{
	PCONTROLLER_DATA data = ControllerGetData(UcxController);
	WdfSpinLockAcquire(data->ChannelMaskLock);
	InterlockedAnd(&data->ChannelMask, ~(1 << Channel));
	WdfSpinLockRelease(data->ChannelMaskLock);
}

NTSTATUS
TR_RunTrSm(
	PTR_DATA TrData,
	BOOLEAN IsInterrupt
);

#define ADDRESS TrData->EndpointHandle->UsbDeviceHandle->Address
#define ENDPOINT TrData->EndpointHandle->UsbEndpointDescriptor.bEndpointAddress & USB_ENDPOINT_ADDRESS_MASK

NTSTATUS
TR_RunChSm(
	PTR_DATA TrData,
	BOOLEAN IsInterrupt
)
{
	NTSTATUS status;
	WdfSpinLockAcquire(TrData->SpinLock);

	__try
	{
		// ignore spurious interrupts outside TRSM_TransferWaiting
		if (IsInterrupt && TrData->TrStateMachine.State != TRSM_TransferWaiting)
			return STATUS_PENDING;

		while (1)
		{
			switch (TrData->StateMachine.State)
			{
			case CHSM_Idle:
				return STATUS_SUCCESS; // "vacuously successful" - there surely is a better status code for this...
			case CHSM_ControlSetup:
			{
				KdPrint((__FUNCTION__ "(%d, %d): CHSM_ControlSetup %02x %02x %02x %02x %02x %02x %02x %02x\n",
					ADDRESS, ENDPOINT,
					TrData->StateMachine.Urb->UrbControlTransfer.SetupPacket[0],
					TrData->StateMachine.Urb->UrbControlTransfer.SetupPacket[1],
					TrData->StateMachine.Urb->UrbControlTransfer.SetupPacket[2],
					TrData->StateMachine.Urb->UrbControlTransfer.SetupPacket[3],
					TrData->StateMachine.Urb->UrbControlTransfer.SetupPacket[4],
					TrData->StateMachine.Urb->UrbControlTransfer.SetupPacket[5],
					TrData->StateMachine.Urb->UrbControlTransfer.SetupPacket[6],
					TrData->StateMachine.Urb->UrbControlTransfer.SetupPacket[7]
					));

				TrData->TrStateMachine.State = TRSM_Init;
				TrData->TrStateMachine.Strikes = 0;
				TrData->TrStateMachine.Done = 0;
				TrData->TrStateMachine.Pid = DWC_HCTSIZ_SETUP;
				TrData->TrStateMachine.Buffer = TrData->StateMachine.Urb->UrbControlTransfer.SetupPacket;
				TrData->TrStateMachine.Length = 8;
				TrData->TrStateMachine.In = FALSE;
				TrData->TrStateMachine.Channel = TrData->StateMachine.Channel;

				TrData->StateMachine.State = CHSM_ControlSetupWait;
				break;
			}
			case CHSM_ControlSetupWait:
				KdPrint((__FUNCTION__ "(%d, %d): CHSM_ControlSetupWait\n", ADDRESS, ENDPOINT));

				status = TR_RunTrSm(TrData, IsInterrupt);

				if (TrData->TrStateMachine.State != TRSM_Done)
				{
					return status;
				}

				TrData->StateMachine.State = CHSM_ControlSetupDone;

				break;
			case CHSM_ControlSetupDone:
				KdPrint((__FUNCTION__ "(%d, %d): CHSM_ControlSetupDone\n", ADDRESS, ENDPOINT));

				if (TrData->StateMachine.Urb->UrbControlTransfer.TransferBufferLength)
				{
					TrData->StateMachine.State = CHSM_ControlData;
				}
				else
				{
					TrData->StateMachine.State = CHSM_ControlStatus;
				}
				IsInterrupt = 0;
				break;
			case CHSM_ControlData:
			{
				// in?
				INT in = TrData->StateMachine.Urb->UrbControlTransfer.TransferFlags & USBD_TRANSFER_DIRECTION_IN;

				KdPrint((__FUNCTION__ "(%d, %d): CHSM_ControlData\n", ADDRESS, ENDPOINT));

				PVOID transferBuffer = TrData->StateMachine.Urb->UrbControlTransfer.TransferBuffer;

				if (TrData->StateMachine.Urb->UrbControlTransfer.TransferBufferMDL)
				{
					transferBuffer = MmGetSystemAddressForMdlSafe(TrData->StateMachine.Urb->UrbControlTransfer.TransferBufferMDL, HighPagePriority);
				}

				TrData->TrStateMachine.State = TRSM_Init;
				TrData->TrStateMachine.Strikes = 0;
				TrData->TrStateMachine.Done = 0;
				TrData->TrStateMachine.Pid = DWC_HCTSIZ_DATA1;
				TrData->TrStateMachine.Buffer = transferBuffer;
				TrData->TrStateMachine.Length = TrData->StateMachine.Urb->UrbControlTransfer.TransferBufferLength;
				TrData->TrStateMachine.In = in;
				TrData->TrStateMachine.Channel = TrData->StateMachine.Channel;

				TrData->StateMachine.State = CHSM_ControlDataWait;
				break;
			}
			case CHSM_ControlDataWait:
				KdPrint((__FUNCTION__ "(%d, %d): CHSM_ControlDataWait\n", ADDRESS, ENDPOINT));

				status = TR_RunTrSm(TrData, IsInterrupt);

				if (TrData->TrStateMachine.State != TRSM_Done)
				{
					return status;
				}

				TrData->StateMachine.State = CHSM_ControlDataDone;

				break;
			case CHSM_ControlDataDone:
				KdPrint((__FUNCTION__ "(%d, %d): CHSM_ControlDataDone\n", ADDRESS, ENDPOINT));

				TrData->StateMachine.State = CHSM_ControlStatus;
				IsInterrupt = 0;
				break;
			case CHSM_ControlStatus:
			{
				// in?
				INT in = TrData->StateMachine.Urb->UrbControlTransfer.TransferFlags & USBD_TRANSFER_DIRECTION_IN;

				KdPrint((__FUNCTION__ "(%d, %d): CHSM_ControlStatus\n", ADDRESS, ENDPOINT));

				TrData->TrStateMachine.State = TRSM_Init;
				TrData->TrStateMachine.Strikes = 0;
				TrData->TrStateMachine.Done = 0;
				TrData->TrStateMachine.Pid = DWC_HCTSIZ_DATA1;
				TrData->TrStateMachine.Buffer = TrData->StatusBuffer;
				TrData->TrStateMachine.Length = 0;
				TrData->TrStateMachine.In = (TrData->StateMachine.Urb->UrbControlTransfer.TransferBufferLength) ? !in : TRUE;
				TrData->TrStateMachine.Channel = TrData->StateMachine.Channel;

				TrData->StateMachine.State = CHSM_ControlStatusWait;
				break;
			}
			case CHSM_ControlStatusWait:
				KdPrint((__FUNCTION__ "(%d, %d): CHSM_ControlStatusWait\n", ADDRESS, ENDPOINT));

				status = TR_RunTrSm(TrData, IsInterrupt);

				if (TrData->TrStateMachine.State != TRSM_Done)
				{
					return status;
				}

				TrData->StateMachine.State = CHSM_ControlStatusDone;

				break;
			case CHSM_ControlStatusDone:
			{
				KdPrint((__FUNCTION__ "(%d, %d): CHSM_ControlStatusDone\n", ADDRESS, ENDPOINT));

				TrData->StateMachine.State = CHSM_Idle;

				TrData->StateMachine.Urb->UrbControlTransfer.Hdr.Status = USBD_STATUS_SUCCESS;
				//Controller_ReleaseChannel(TrData->EndpointHandle->UsbDeviceHandle->UcxController, TrData->StateMachine.Channel);

				WdfRequestComplete(TrData->StateMachine.Request, STATUS_SUCCESS);
				//TrData->StateMachine.State = CHSM_ControlStatus;
				return STATUS_SUCCESS;
			}
			case CHSM_AddressSetup:
			{
				static USB_DEFAULT_PIPE_SETUP_PACKET setupPacket;

				KdPrint((__FUNCTION__ "(%d, %d): CHSM_AddressSetup\n", ADDRESS, ENDPOINT));

#define USBPORT_INIT_SETUP_PACKET(s, brequest, \
    direction, recipient, typ, wvalue, windex, wlength) \
    {\
    (s).bRequest = (brequest);\
    (s).bmRequestType.Dir = (direction);\
    (s).bmRequestType.Type = (typ);\
    (s).bmRequestType.Recipient = (recipient);\
    (s).bmRequestType.Reserved = 0;\
    (s).wValue.W = (wvalue);\
    (s).wIndex.W = (windex);\
    (s).wLength = (wlength);\
    }

				USBPORT_INIT_SETUP_PACKET(setupPacket,
					USB_REQUEST_SET_ADDRESS, // bRequest
					BMREQUEST_HOST_TO_DEVICE, // Dir
					BMREQUEST_TO_DEVICE, // Recipient
					BMREQUEST_STANDARD, // Type
					TrData->StateMachine.Address, // wValue
					0, // wIndex
					0)

				TrData->TrStateMachine.State = TRSM_Init;
				TrData->TrStateMachine.Strikes = 0;
				TrData->TrStateMachine.Done = 0;
				TrData->TrStateMachine.Pid = DWC_HCTSIZ_SETUP;
				TrData->TrStateMachine.Buffer = &setupPacket;
				TrData->TrStateMachine.Length = 8;
				TrData->TrStateMachine.In = FALSE;
				TrData->TrStateMachine.Channel = TrData->StateMachine.Channel;

				TrData->StateMachine.Urb = NULL;

				TrData->StateMachine.State = CHSM_AddressSetupWait;
				break;
			}
			case CHSM_AddressSetupWait:
				KdPrint((__FUNCTION__ "(%d, %d): CHSM_AddressSetupWait\n", ADDRESS, ENDPOINT));

				status = TR_RunTrSm(TrData, IsInterrupt);

				if (TrData->TrStateMachine.State != TRSM_Done)
				{
					return status;
				}

				TrData->StateMachine.State = CHSM_AddressSetupDone;

				break;
			case CHSM_AddressSetupDone:
				KdPrint((__FUNCTION__ "(%d, %d): CHSM_AddressSetupDone\n", ADDRESS, ENDPOINT));

				TrData->StateMachine.State = CHSM_AddressStatus;
				IsInterrupt = 0;
				break;
			case CHSM_AddressStatus:
				KdPrint((__FUNCTION__ "(%d, %d): CHSM_AddressStatus\n", ADDRESS, ENDPOINT));

				TrData->TrStateMachine.State = TRSM_Init;
				TrData->TrStateMachine.Strikes = 0;
				TrData->TrStateMachine.Done = 0;
				TrData->TrStateMachine.Pid = DWC_HCTSIZ_DATA1;
				TrData->TrStateMachine.Buffer = TrData->StatusBuffer;
				TrData->TrStateMachine.Length = 0;
				TrData->TrStateMachine.In = TRUE;
				TrData->TrStateMachine.Channel = TrData->StateMachine.Channel;

				TrData->StateMachine.State = CHSM_AddressStatusWait;
				break;
			case CHSM_AddressStatusWait:
				KdPrint((__FUNCTION__ "(%d, %d): CHSM_AddressStatusWait\n", ADDRESS, ENDPOINT));

				status = TR_RunTrSm(TrData, IsInterrupt);

				if (TrData->TrStateMachine.State != TRSM_Done)
				{
					return status;
				}

				TrData->StateMachine.State = CHSM_AddressStatusDone;

				break;
			case CHSM_AddressStatusDone:
			{
				KdPrint((__FUNCTION__ "(%d, %d): CHSM_AddressStatusDone\n", ADDRESS, ENDPOINT));

				TrData->EndpointHandle->UsbDeviceHandle->Address = TrData->StateMachine.Address;

				TrData->StateMachine.State = CHSM_Idle;
				//Controller_ReleaseChannel(TrData->EndpointHandle->UsbDeviceHandle->UcxController, TrData->StateMachine.Channel);

				WdfRequestComplete(TrData->StateMachine.Request, STATUS_SUCCESS);
				//TrData->StateMachine.State = CHSM_ControlStatus;
				return STATUS_SUCCESS;
			}
			case CHSM_InterruptOrBulkData:
			{
				// in?
				INT in = TrData->StateMachine.Urb->UrbBulkOrInterruptTransfer.TransferFlags & USBD_TRANSFER_DIRECTION_IN;

				KdPrint((__FUNCTION__ "(%d, %d): CHSM_InterruptOrBulkData\n", ADDRESS, ENDPOINT));

				PVOID transferBuffer = TrData->StateMachine.Urb->UrbBulkOrInterruptTransfer.TransferBuffer;

				if (TrData->StateMachine.Urb->UrbBulkOrInterruptTransfer.TransferBufferMDL)
				{
					transferBuffer = MmGetSystemAddressForMdlSafe(TrData->StateMachine.Urb->UrbBulkOrInterruptTransfer.TransferBufferMDL, HighPagePriority);
				}

				TrData->TrStateMachine.State = TRSM_Init;
				TrData->TrStateMachine.Strikes = 0;
				TrData->TrStateMachine.Done = 0;
				TrData->TrStateMachine.Pid = (in) ? TrData->EndpointHandle->InToggle : TrData->EndpointHandle->OutToggle;
				TrData->TrStateMachine.Buffer = transferBuffer;
				TrData->TrStateMachine.Length = TrData->StateMachine.Urb->UrbBulkOrInterruptTransfer.TransferBufferLength;
				TrData->TrStateMachine.In = in;
				TrData->TrStateMachine.Channel = TrData->StateMachine.Channel;

				TrData->StateMachine.State = CHSM_InterruptOrBulkDataWait;
				break;
			}
			case CHSM_InterruptOrBulkDataWait:
				KdPrint((__FUNCTION__ "(%d, %d): CHSM_InterruptOrBulkDataWait\n", ADDRESS, ENDPOINT));

				status = TR_RunTrSm(TrData, IsInterrupt);

				if (TrData->TrStateMachine.State != TRSM_Done)
				{
					return status;
				}

				TrData->StateMachine.State = CHSM_InterruptOrBulkDataDone;

				break;
			case CHSM_InterruptOrBulkDataDone:
			{
				KdPrint((__FUNCTION__ "(%d, %d): CHSM_InterruptOrBulkDataDone\n", ADDRESS, ENDPOINT));

				INT in = TrData->StateMachine.Urb->UrbBulkOrInterruptTransfer.TransferFlags & USBD_TRANSFER_DIRECTION_IN;

				if (in)
				{
					TrData->EndpointHandle->InToggle = TrData->TrStateMachine.Pid;
				}
				else
				{
					TrData->EndpointHandle->OutToggle = TrData->TrStateMachine.Pid;
				}

				TrData->StateMachine.State = CHSM_Idle;
				//Controller_ReleaseChannel(TrData->EndpointHandle->UsbDeviceHandle->UcxController, TrData->StateMachine.Channel);

				TrData->StateMachine.Urb->UrbBulkOrInterruptTransfer.Hdr.Status = USBD_STATUS_SUCCESS;
				WdfRequestComplete(TrData->StateMachine.Request, STATUS_SUCCESS);

				return STATUS_SUCCESS;
			}
			}
		}
	}
	__finally
	{
		WdfSpinLockRelease(TrData->SpinLock);
	}
}

VOID
ReviveTrSm(
	PTR_DATA TrData
)
{
	int channel = TrData->TrStateMachine.Channel;

	PCONTROLLER_DATA controllerData = ControllerGetData(TrData->EndpointHandle->UsbDeviceHandle->UcxController);
	if (channel != -1)
	{
		controllerData->ChTtHubs[channel] = -1;
		controllerData->ChTtPorts[channel] = -1;
	}

	for (int i = 0; i < NUM_CHANNELS; i++)
	{
		PTR_DATA chanData = controllerData->ChTrDatas[i];

		if (chanData)
		{
			if (chanData->TrStateMachine.State == TRSM_CheckFreePort &&
				chanData->TrStateMachine.TtPort == TrData->TrStateMachine.TtPort &&
				chanData->TrStateMachine.TtHub == TrData->TrStateMachine.TtHub)
			{
				KdPrint(("Reviving channel %d\n", i));

				chanData->EndpointHandle->EpResumeContext = chanData;

				ExSetTimer(
					chanData->EndpointHandle->EpResumeTimer,
					WDF_REL_TIMEOUT_IN_US(50),
					0,
					NULL
				);

				break;
			}
		}
	}
}

INT TR_PollingInterval(PTR_DATA TrData)
{
	int interval = TrData->EndpointHandle->UsbEndpointDescriptor.bInterval;
	if (!interval)
		interval = 1;

	switch (TrData->EndpointHandle->Type)
	{
	case EndpointType_Control:
	case EndpointType_Interrupt:
		switch (TrData->EndpointHandle->UsbDeviceHandle->UsbDeviceInfo.DeviceSpeed)
		{
		case UsbLowSpeed:
			return 8000 * max((interval + 7) / 8, 1);
		case UsbFullSpeed:
			return 1000 * max(interval, 1);
		case UsbHighSpeed:
			return 125 * (1 << (min(interval, 16) - 1));
		}
		break;
	case EndpointType_Bulk:
		switch (TrData->EndpointHandle->UsbDeviceHandle->UsbDeviceInfo.DeviceSpeed)
		{
		case UsbLowSpeed: // forbidden by spec; fall through to full-speed case
		case UsbFullSpeed:
			return 1000 * max(interval, 1);
		case UsbHighSpeed:
			return 125 * max(interval, 1);
		}
		break;
	case EndpointType_Isoch:
		switch (TrData->EndpointHandle->UsbDeviceHandle->UsbDeviceInfo.DeviceSpeed)
		{
		case UsbLowSpeed: // forbidden by spec; fall through to full-speed case
		case UsbFullSpeed:
			return 1000 * (1 << (min(interval, 16) - 1));
		case UsbHighSpeed:
			return 125 * (1 << (min(interval, 16) - 1));
		}
		break;
	}
	return 1000; // a safe default if we somehow end up here
}

VOID
Controller_RunCHSM(
	PVOID Context
);

NTSTATUS
TR_RunTrSm(
	PTR_DATA TrData,
	BOOLEAN IsInterrupt
)
{
	PCONTROLLER_DATA controllerHandle;

	while (1)
	{
		switch (TrData->TrStateMachine.State)
		{
		case TRSM_Init:
		{
			int channel = TrData->TrStateMachine.Channel;
			if (channel == -1) {
				NTSTATUS status = Controller_AllocateChannel(TrData->EndpointHandle->UsbDeviceHandle->UcxController, &channel);
				if (!NT_SUCCESS(status)) {
					WdfRequestComplete(TrData->StateMachine.Request, status);
					return status;
				}
				TrData->EndpointHandle->UsbDeviceHandle->HostGlobalRegs->haintmsk &= ~(1 << TrData->TrStateMachine.Channel);
				Controller_SetChannelCallback(TrData->EndpointHandle->UsbDeviceHandle->UcxController, channel, Controller_RunCHSM, TrData);
				TrData->StateMachine.Channel = TrData->TrStateMachine.Channel = channel;
				TrData->TrStateMachine.Strikes = 0;
			}

			dwc_otg_hc_regs_t* regs = TrData->EndpointHandle->UsbDeviceHandle->ChannelRegs[channel];

			if (TrData->TrStateMachine.Strikes >= 3)
			{
				KdPrint((__FUNCTION__ "(%d, %d): TRSM_Init: three strikes, reporting error (%x, %x, %x, %x, %x, %x, %x)\n", ADDRESS, ENDPOINT, regs->hcchar, regs->hcdma, regs->hcdmab, regs->hcint, regs->hcintmsk, regs->hcsplt, regs->hctsiz));
				KdPrint((__FUNCTION__ "(%d, %d): TRSM_Init: releasing channel %d\n", ADDRESS, ENDPOINT, TrData->TrStateMachine.Channel));

				if (TrData->StateMachine.Urb)
				{
					TrData->StateMachine.Urb->UrbHeader.Status = USBD_STATUS_TIMEOUT;
				}

				TrData->StateMachine.State = CHSM_Idle;

				controllerHandle = ControllerGetData(TrData->EndpointHandle->UsbDeviceHandle->UcxController);
				controllerHandle->ChTtHubs[TrData->TrStateMachine.Channel] = 0;
				controllerHandle->ChTtPorts[TrData->TrStateMachine.Channel] = 0;

				controllerHandle->ChTrDatas[TrData->TrStateMachine.Channel] = NULL;

				TrData->EndpointHandle->UsbDeviceHandle->HostGlobalRegs->haintmsk &= ~(1 << TrData->TrStateMachine.Channel);
				Controller_SetChannelCallback(TrData->EndpointHandle->UsbDeviceHandle->UcxController, channel, Controller_RunCHSM, TrData);
				Controller_ReleaseChannel(TrData->EndpointHandle->UsbDeviceHandle->UcxController, TrData->TrStateMachine.Channel);
				TrData->StateMachine.Channel = TrData->TrStateMachine.Channel = -1;
				WdfRequestComplete(TrData->StateMachine.Request, STATUS_TIMEOUT);

				return STATUS_TIMEOUT;
			}

			KdPrint((__FUNCTION__ "(%d, %d): TRSM_Init: using channel %d\n", ADDRESS, ENDPOINT, TrData->TrStateMachine.Channel));

			KeMemoryBarrier();
			_DataSynchronizationBarrier();

			hcchar_data_t hcchar;
			hcchar.d32 = regs->hcchar;
			hcchar.b.chen = 0;
			hcchar.b.chdis = 1;
			hcchar.b.epdir = 0;

			_DataSynchronizationBarrier();

			regs->hcchar = hcchar.d32;

			KeMemoryBarrier();
			_DataSynchronizationBarrier();

			hcchar.d32 = regs->hcchar;
			hcchar.b.chen = 1;
			hcchar.b.chdis = 1;
			hcchar.b.epdir = 0;

			_DataSynchronizationBarrier();

			regs->hcchar = hcchar.d32;
			LARGE_INTEGER delay;
			delay.QuadPart = 10; // times 100 nanoseconds == 1 us

			for (int i = 0; i < 10000; i++) { // dwc_otg has 1000, but that's not enough safety margin; I've seen channels reset only after 300+ iterations
				KeMemoryBarrier();
				_DataSynchronizationBarrier();

				hcchar.d32 = regs->hcchar;
				if (!hcchar.b.chen) {
					KdPrint((__FUNCTION__ "(%d, %d): TRSM_Init: channel reset took %d iterations\n", ADDRESS, ENDPOINT, i));
					break;
				}

				KeDelayExecutionThread(KernelMode, FALSE, &delay);
			}

			if (hcchar.b.chen) {
				KdPrint((__FUNCTION__ "(%d, %d): TRSM_Init: channel reset failed, abandoning channel %d\n", ADDRESS, ENDPOINT, channel));
				KdPrint((__FUNCTION__ "(%d, %d): TRSM_Init: registers at failure: %x, %x, %x, %x, %x, %x, %x\n", ADDRESS, ENDPOINT, regs->hcchar, regs->hcdma, regs->hcdmab, regs->hcint, regs->hcintmsk, regs->hcsplt, regs->hctsiz));

				controllerHandle = ControllerGetData(TrData->EndpointHandle->UsbDeviceHandle->UcxController);
				controllerHandle->ChTtHubs[TrData->TrStateMachine.Channel] = 0;
				controllerHandle->ChTtPorts[TrData->TrStateMachine.Channel] = 0;

				controllerHandle->ChTrDatas[TrData->TrStateMachine.Channel] = NULL;
				Controller_SetChannelCallback(TrData->EndpointHandle->UsbDeviceHandle->UcxController, channel, NULL, NULL);
				// intentionally leak this channel so we don't run into it again
				controllerHandle->DeadChannelMask |= (1 << channel);

				NTSTATUS status = Controller_AllocateChannel(TrData->EndpointHandle->UsbDeviceHandle->UcxController, &channel);
				if (!NT_SUCCESS(status)) {
					KdPrint((__FUNCTION__ "(%d, %d): TRSM_Init: out of channels, reporting error\n", ADDRESS, ENDPOINT));
					if (TrData->StateMachine.Urb)
					{
						TrData->StateMachine.Urb->UrbHeader.Status = USBD_STATUS_XACT_ERROR;
					}
					WdfRequestComplete(TrData->StateMachine.Request, STATUS_UNSUCCESSFUL);
					return STATUS_UNSUCCESSFUL;
				}
				Controller_SetChannelCallback(TrData->EndpointHandle->UsbDeviceHandle->UcxController, channel, Controller_RunCHSM, TrData);
				TrData->StateMachine.Channel = TrData->TrStateMachine.Channel = channel;

				regs = TrData->EndpointHandle->UsbDeviceHandle->ChannelRegs[channel];
			}

			int max = TrData->EndpointHandle->MaxPacketSize;
			int ep = TrData->EndpointHandle->UsbEndpointDescriptor.bEndpointAddress & USB_ENDPOINT_ADDRESS_MASK;
			int devnum = TrData->EndpointHandle->UsbDeviceHandle->Address;

			TrData->TrStateMachine.DoSplit = 0;
			TrData->TrStateMachine.CompleteSplit = 0;
			//TrData->TrStateMachine.Done = 0;
			TrData->TrStateMachine.SSplitFrameNum = 0;
			TrData->TrStateMachine.IsRetry = 0;

			TrData->TrStateMachine.MaxXferLen = 511 * max;

			if (TrData->TrStateMachine.MaxXferLen > 65536)
			{
				TrData->TrStateMachine.MaxXferLen = 65536;
			}

			TrData->TrStateMachine.NumPackets = TrData->TrStateMachine.MaxXferLen / max;
			TrData->TrStateMachine.MaxXferLen = TrData->TrStateMachine.NumPackets * max;

			controllerHandle = ControllerGetData(TrData->EndpointHandle->UsbDeviceHandle->UcxController);
			controllerHandle->ChTrDatas[channel] = TrData;

			hcchar.d32 = 0;
			hcchar.b.devaddr = devnum;
			hcchar.b.epnum = ep;
			hcchar.b.epdir = TrData->TrStateMachine.In;
			if (TrData->EndpointHandle->Type == EndpointType_Interrupt)
			{
				hcchar.b.eptype = 3;
			}
			else if (TrData->EndpointHandle->Type == EndpointType_Control)
			{
				hcchar.b.eptype = 0; // TODO: HARDCODED CONTROL
			}
			else if (TrData->EndpointHandle->Type == EndpointType_Bulk)
			{
				hcchar.b.eptype = 2;
			}
			else if (TrData->EndpointHandle->Type == EndpointType_Isoch)
			{
				hcchar.b.eptype = 1;
			}
			hcchar.b.mps = max;
			hcchar.b.lspddev = 0;

			// TODO: set lspddev if low-speed
			if (TrData->EndpointHandle->UsbDeviceHandle->UsbDeviceInfo.DeviceSpeed == UsbLowSpeed)
			{
				KdPrint((__FUNCTION__ "(%d, %d): low speed device\n", ADDRESS, ENDPOINT));

				hcchar.b.lspddev = 1;
			}

			KeMemoryBarrier();
			_DataSynchronizationBarrier();

			regs->hcchar = hcchar.d32;

			KeMemoryBarrier();
			_DataSynchronizationBarrier();

			regs->hcsplt = 0;

			// TODO: init split if full/low-speed
			PUCXUSBDEVICE_INFO usbDeviceInfo = &TrData->EndpointHandle->UsbDeviceHandle->UsbDeviceInfo;

			if (usbDeviceInfo->DeviceSpeed == UsbLowSpeed ||
				usbDeviceInfo->DeviceSpeed == UsbFullSpeed)
			{
				if (usbDeviceInfo->TtHub != NULL)
				{
					PUSBDEVICE_DATA ttHubData = GetUsbDeviceData(usbDeviceInfo->TtHub);
					UINT8 translatorHubAddress = (UINT8)ttHubData->Address;
					UINT8 translatorPortNumber = 0;

					if (usbDeviceInfo->PortPath.TTHubDepth != 0)
					{
						translatorPortNumber =
							(UINT8)usbDeviceInfo->PortPath.PortPath[usbDeviceInfo->PortPath.TTHubDepth];
					}
					else
					{
						KdPrint((__FUNCTION__ "(%d, %d): no translator port?\n", ADDRESS, ENDPOINT));
					}

					TrData->TrStateMachine.DoSplit = 1;
					TrData->TrStateMachine.NumPackets = 1;
					TrData->TrStateMachine.MaxXferLen = max;

					KdPrint((__FUNCTION__ "(%d, %d): split transfer (hub %d, port %d)\n", ADDRESS, ENDPOINT, translatorHubAddress, translatorPortNumber));

					TrData->TrStateMachine.TtHub = translatorHubAddress;
					TrData->TrStateMachine.TtPort = translatorPortNumber;

					TrData->TrStateMachine.State = TRSM_CheckFreePort;
					break;
				}
				else
				{
					KdPrint((__FUNCTION__ "(%d, %d): TtHub is NULL?\n", ADDRESS, ENDPOINT));
				}
			}

			TrData->TrStateMachine.State = TRSM_Transferring;
			break;
		}
		case TRSM_CheckFreePort:
		{
			KdPrint((__FUNCTION__ "(%d, %d): TRSM_CheckFreePort", ADDRESS, ENDPOINT));

			controllerHandle = ControllerGetData(TrData->EndpointHandle->UsbDeviceHandle->UcxController);

			BOOLEAN foundSelf = FALSE;

			for (int i = 0; i < NUM_CHANNELS; i++)
			{
				if (controllerHandle->ChTtHubs[i] == TrData->TrStateMachine.TtHub &&
					controllerHandle->ChTtPorts[i] == TrData->TrStateMachine.TtPort)
				{
					foundSelf = TRUE;
					KdPrint((__FUNCTION__ "(%d, %d): FoundSelf", ADDRESS, ENDPOINT));
				}
			}

			if (!foundSelf)
			{
				controllerHandle->ChTtHubs[TrData->TrStateMachine.Channel] = TrData->TrStateMachine.TtHub;
				controllerHandle->ChTtPorts[TrData->TrStateMachine.Channel] = TrData->TrStateMachine.TtPort;

				dwc_otg_hc_regs_t* regs = TrData->EndpointHandle->UsbDeviceHandle->ChannelRegs[TrData->TrStateMachine.Channel];

				hcsplt_data_t hcsplt;
				hcsplt.d32 = 0;

				hcsplt.b.spltena = 1;
				hcsplt.b.hubaddr = TrData->TrStateMachine.TtHub;
				hcsplt.b.prtaddr = TrData->TrStateMachine.TtPort;

				KeMemoryBarrier();
				_DataSynchronizationBarrier();

				regs->hcsplt = hcsplt.d32;

				KeMemoryBarrier();
				_DataSynchronizationBarrier();

				TrData->TrStateMachine.State = TRSM_Transferring;
				break;
			}

			controllerHandle->ChTtHubs[TrData->TrStateMachine.Channel] = 0;
			controllerHandle->ChTtPorts[TrData->TrStateMachine.Channel] = 0;

			controllerHandle->ChTrDatas[TrData->TrStateMachine.Channel] = NULL;
			Controller_SetChannelCallback(TrData->EndpointHandle->UsbDeviceHandle->UcxController, TrData->TrStateMachine.Channel, NULL, NULL);
			KdPrint((__FUNCTION__ "(%d, %d): TRSM_CheckFreePort: releasing channel %d\n", ADDRESS, ENDPOINT, TrData->TrStateMachine.Channel));
			TrData->EndpointHandle->UsbDeviceHandle->HostGlobalRegs->haintmsk &= ~(1 << TrData->TrStateMachine.Channel);
			Controller_ReleaseChannel(TrData->EndpointHandle->UsbDeviceHandle->UcxController, TrData->TrStateMachine.Channel);
			TrData->StateMachine.Channel = TrData->TrStateMachine.Channel = -1;
			return STATUS_DEVICE_ALREADY_ATTACHED;
		}
		case TRSM_Transferring:
		{
			hfnum_data_t hfnum;
			hfnum.d32 = READ_REGISTER_ULONG((volatile ULONG*)&TrData->EndpointHandle->UsbDeviceHandle->HostGlobalRegs->hfnum);

			KdPrint((__FUNCTION__ "(%d, %d): TRSM_Transferring (done: %d, length: %d, split: %d, compsplit: %d, address: %d, frnum: %d, subfr: %d, direction: %d)\n",
				ADDRESS, ENDPOINT,
				TrData->TrStateMachine.Done,
				TrData->TrStateMachine.Length,
				TrData->TrStateMachine.DoSplit,
				TrData->TrStateMachine.CompleteSplit,
				TrData->EndpointHandle->UsbDeviceHandle->Address,
				hfnum.b.frnum,
				hfnum.b.frnum & 7,
				TrData->TrStateMachine.In));

			ULONG max = TrData->EndpointHandle->MaxPacketSize;

			TrData->TrStateMachine.XferLen = TrData->TrStateMachine.Length - TrData->TrStateMachine.Done;

			if (TrData->TrStateMachine.XferLen > TrData->TrStateMachine.MaxXferLen)
			{
				TrData->TrStateMachine.XferLen = TrData->TrStateMachine.MaxXferLen;
			}
			else if (TrData->TrStateMachine.MaxXferLen > max)
			{
				TrData->TrStateMachine.NumPackets = (TrData->TrStateMachine.XferLen + max - 1) / max;
			}
			else
			{
				TrData->TrStateMachine.NumPackets = 1;
			}

			if (TrData->TrStateMachine.Length == 0 || TrData->TrStateMachine.XferLen == 0)
			{
				TrData->TrStateMachine.NumPackets = 1;
			}

			int channel = TrData->TrStateMachine.Channel;
			dwc_otg_hc_regs_t* regs = TrData->EndpointHandle->UsbDeviceHandle->ChannelRegs[channel];

			hcsplt_data_t hcsplt;

			KeMemoryBarrier();
			_DataSynchronizationBarrier();

			hcsplt.d32 = regs->hcsplt;

			KeMemoryBarrier();
			_DataSynchronizationBarrier();

			if (TrData->TrStateMachine.CompleteSplit)
			{
				KdPrint((__FUNCTION__ "(%d, %d): CompleteSplit: was %x\n", ADDRESS, ENDPOINT, hcsplt.d32));

				hcsplt.b.compsplt = 1;
			}
			else if (TrData->TrStateMachine.DoSplit)
			{
				hcsplt.b.compsplt = 0;
			}

			KeMemoryBarrier();
			_DataSynchronizationBarrier();

			regs->hcsplt = hcsplt.d32;

			KeMemoryBarrier();

			// top half of transfer_chunk
			hctsiz_data_t hctsiz;
			hctsiz.d32 = 0;
			hctsiz.b.xfersize = TrData->TrStateMachine.XferLen;
			hctsiz.b.pktcnt = TrData->TrStateMachine.NumPackets;
			hctsiz.b.pid = TrData->TrStateMachine.Pid;

			KeMemoryBarrier();
			_DataSynchronizationBarrier();

			regs->hctsiz = hctsiz.d32;

			controllerHandle = ControllerGetData(TrData->EndpointHandle->UsbDeviceHandle->UcxController);

			if (TrData->TrStateMachine.XferLen)
			{
				if (!TrData->TrStateMachine.In)
				{
					RtlCopyMemory(controllerHandle->CommonBufferBase[TrData->TrStateMachine.Channel],
						(PCHAR)TrData->TrStateMachine.Buffer + TrData->TrStateMachine.Done,
						TrData->TrStateMachine.XferLen);

					KeMemoryBarrier();
					_DataSynchronizationBarrier();
				}
			}

			WRITE_REGISTER_ULONG((volatile ULONG*)&regs->hcdma, (ULONG)controllerHandle->CommonBufferBaseLA[TrData->TrStateMachine.Channel].QuadPart);
			regs->hcint = 0x3FFF;

			_DataSynchronizationBarrier();

			hcintmsk_data_t hcintmsk;
			hcintmsk.d32 = 0;
			hcintmsk.b.chhltd = 1;
			/*hcintmsk.b.ack = 1;
			hcintmsk.b.bblerr = 1;
			hcintmsk.b.datatglerr = 1;
			hcintmsk.b.frmovrun = 1;
			hcintmsk.b.nak = 1;
			hcintmsk.b.nyet = 1;
			hcintmsk.b.stall = 1;
			hcintmsk.b.xacterr = 1;
			hcintmsk.b.xfercompl = 1;*/

			_DataSynchronizationBarrier();

			regs->hcintmsk = hcintmsk.d32;

			// enable interrupts for this channel
			TrData->EndpointHandle->UsbDeviceHandle->HostGlobalRegs->haintmsk |= (1 << TrData->TrStateMachine.Channel);

			hcchar_data_t hcchar;
			hcchar.d32 = regs->hcchar;

			_DataSynchronizationBarrier();

			hcchar.b.multicnt = 0;
			hcchar.b.chen = 0;
			hcchar.b.chdis = 0;
			hcchar.b.oddfrm = 0;

			hcchar.b.multicnt = 1;
			hcchar.b.oddfrm = 0;
			hcchar.b.chen = 1;

			if (TrData->EndpointHandle->Type == EndpointType_Interrupt)
			{
				hcchar.b.oddfrm = (!(TrData->EndpointHandle->UsbDeviceHandle->HostGlobalRegs->hfnum & 1));
			}

			_DataSynchronizationBarrier();

			regs->hcchar = hcchar.d32;

			KeMemoryBarrier();
			_DataSynchronizationBarrier();

			TrData->TrStateMachine.State = TRSM_TransferWaiting;
			TrData->TrStateMachine.IsRetry = 0;

			//return;
			break;
		}
		case TRSM_TransferWaiting:
		{
			KdPrint((__FUNCTION__ "(%d, %d): TRSM_TransferWaiting\n", ADDRESS, ENDPOINT));

			int channel = TrData->TrStateMachine.Channel;
			dwc_otg_hc_regs_t* regs = TrData->EndpointHandle->UsbDeviceHandle->ChannelRegs[channel];
			controllerHandle = ControllerGetData(TrData->EndpointHandle->UsbDeviceHandle->UcxController);

			hcint_data_t hcint;
			hcint_data_t hcintref;
			hcintref.d32 = 0;
			hcintref.b.chhltd = 1;
			/*hcintref.b.ack = 1;
			hcintref.b.bblerr = 1;
			hcintref.b.datatglerr = 1;
			hcintref.b.frmovrun = 1;
			hcintref.b.nak = 1;
			hcintref.b.nyet = 1;
			hcintref.b.stall = 1;
			hcintref.b.xacterr = 1;
			hcintref.b.xfercomp = 1;*/

			KeMemoryBarrier();
			_DataSynchronizationBarrier();

			hcint.d32 = regs->hcint;

			if (hcint.d32 & hcintref.d32)
			{
				TrData->TrStateMachine.State = TRSM_TransferHalted;
				TrData->TrStateMachine.IsRetry = 0;
				ExCancelTimer(TrData->EndpointHandle->EpResumeTimer, NULL);
				break;
			}
			else if (!TrData->TrStateMachine.IsRetry)
			{
				TrData->TrStateMachine.IsRetry = 1;
				TrData->EndpointHandle->EpResumeContext = TrData;

				ExSetTimer(
					TrData->EndpointHandle->EpResumeTimer,
					WDF_REL_TIMEOUT_IN_MS(5000 /*20 << TrData->TrStateMachine.Strikes*/),
					0,
					NULL
				);
			} 
			else if (IsInterrupt)
			{
				KdPrint((__FUNCTION__ "(%d, %d): TRSM_TransferWaiting: spurious interrupt\n", ADDRESS, ENDPOINT));
				return STATUS_PENDING;
			}
			else
			{
				KdPrint((__FUNCTION__ "(%d, %d): TRSM_TransferWaiting: channel timeout, will attempt channel reset\n", ADDRESS, ENDPOINT));

				TrData->TrStateMachine.State = TRSM_Init;
				TrData->TrStateMachine.Strikes++;

				ReviveTrSm(TrData);

				break;

				/*if (TrData->StateMachine.Urb)
				{
					TrData->StateMachine.Urb->UrbHeader.Status = USBD_STATUS_TIMEOUT;
				}

				TrData->StateMachine.State = CHSM_Idle;

				controllerHandle = ControllerGetData(TrData->EndpointHandle->UsbDeviceHandle->UcxController);
				controllerHandle->ChTtHubs[channel] = 0;
				controllerHandle->ChTtPorts[channel] = 0;

				controllerHandle->ChTrDatas[channel] = NULL;

				KdPrint((__FUNCTION__ "(%d, %d): TRSM_TransferWaiting: releasing channel %d\n", ADDRESS, ENDPOINT, TrData->TrStateMachine.Channel));
				Controller_SetChannelCallback(TrData->EndpointHandle->UsbDeviceHandle->UcxController, TrData->TrStateMachine.Channel, NULL, NULL);
				TrData->EndpointHandle->UsbDeviceHandle->HostGlobalRegs->haintmsk &= ~(1 << TrData->TrStateMachine.Channel);
				Controller_ReleaseChannel(TrData->EndpointHandle->UsbDeviceHandle->UcxController, TrData->StateMachine.Channel);
				WdfRequestComplete(TrData->StateMachine.Request, STATUS_TIMEOUT);

				return STATUS_TIMEOUT;*/
			}

			KdPrint((__FUNCTION__ "(%d, %d): TRSM_TransferWaiting: not halted yet\n", ADDRESS, ENDPOINT));
			return STATUS_PENDING;
		}
		case TRSM_TransferHalted:
		{
			KdPrint((__FUNCTION__ "(%d, %d): TRSM_TransferHalted\n", ADDRESS, ENDPOINT));

			int channel = TrData->TrStateMachine.Channel;
			dwc_otg_hc_regs_t* regs = TrData->EndpointHandle->UsbDeviceHandle->ChannelRegs[channel];

			hcint_data_t hcint;

			KeMemoryBarrier();
			_DataSynchronizationBarrier();

			hcint.d32 = regs->hcint;
			regs->hcint = 0x3FFF;

			// TODO: split logic
			BOOLEAN tempCompletedSplit = FALSE;

			if (TrData->TrStateMachine.CompleteSplit)
			{
				if (hcint.b.nyet)
				{
					hfnum_data_t hfnum;

					KeMemoryBarrier();
					_DataSynchronizationBarrier();

					hfnum.d32 = TrData->EndpointHandle->UsbDeviceHandle->HostGlobalRegs->hfnum;

					if (((hfnum.b.frnum - TrData->TrStateMachine.SSplitFrameNum) & 0x3FFF) > 16)
					{
						KdPrint((__FUNCTION__ "(%d, %d): Split NYET timeout, retry\n", ADDRESS, ENDPOINT));

						TrData->TrStateMachine.State = TRSM_Init;
						TrData->TrStateMachine.Strikes++;

						ReviveTrSm(TrData);

						break;
					}
				}
				else
				{
					KdPrint((__FUNCTION__ "(%d, %d): split complete?\n", ADDRESS, ENDPOINT));

					TrData->TrStateMachine.CompleteSplit = 0;

					//tempCompletedSplit = TRUE;
				}
			}
			else if (TrData->TrStateMachine.DoSplit)
			{
				if (hcint.b.ack)
				{
					KdPrint((__FUNCTION__ "(%d, %d): Split ACK, complete it?\n", ADDRESS, ENDPOINT));

					hfnum_data_t hfnum;

					KeMemoryBarrier();
					_DataSynchronizationBarrier();

					hfnum.d32 = TrData->EndpointHandle->UsbDeviceHandle->HostGlobalRegs->hfnum;

					TrData->TrStateMachine.SSplitFrameNum = hfnum.b.frnum;

					TrData->TrStateMachine.CompleteSplit = 1;
				}
			}

			TrData->TrStateMachine.Strikes = 0;

			hctsiz_data_t hctsiz;

			KeMemoryBarrier();
			_DataSynchronizationBarrier();

			hctsiz.d32 = regs->hctsiz;

			TrData->TrStateMachine.Pid = (UINT8)hctsiz.b.pid;

			if (hcint.b.xfercomp || hcint.b.nyet || hcint.b.ack || tempCompletedSplit)
			{
				if (hcint.b.xfercomp)
				{
					KdPrint((__FUNCTION__ "(%d, %d): Halted: XFERCOMP\n", ADDRESS, ENDPOINT));
				}
				else if (hcint.b.ack)
				{
					KdPrint((__FUNCTION__ "(%d, %d): Halted: ACK\n", ADDRESS, ENDPOINT));
				}
				else if (hcint.b.nyet)
				{
					KdPrint((__FUNCTION__ "(%d, %d): Halted: NYET\n", ADDRESS, ENDPOINT));
				}

				ULONG sub = hctsiz.b.xfersize;
				ULONG xfer_len = TrData->TrStateMachine.XferLen;

				// Controller bug: in certain circumstances (device sends too much data?), xfersize can underflow
				if (sub > xfer_len)
				{
					sub = 0;
				}

				/*if (hcint.b.xfercomp)
				{*/
					xfer_len -= sub;

					if (TrData->TrStateMachine.In)
					{
						controllerHandle = ControllerGetData(TrData->EndpointHandle->UsbDeviceHandle->UcxController);

						RtlCopyMemory((PCHAR)TrData->TrStateMachine.Buffer + TrData->TrStateMachine.Done,
							controllerHandle->CommonBufferBase[TrData->TrStateMachine.Channel],
							xfer_len);

						_DataSynchronizationBarrier();
						KeMemoryBarrier();
					}
				//}
				
				TrData->TrStateMachine.Done += xfer_len;
				KdPrint((__FUNCTION__ "(%d, %d): Transferred %d bytes, %d remaining\n", ADDRESS, ENDPOINT, xfer_len, TrData->TrStateMachine.Length - TrData->TrStateMachine.Done));

				if (!TrData->TrStateMachine.CompleteSplit)
				{
					if (xfer_len < TrData->EndpointHandle->MaxPacketSize)
					{
						TrData->TrStateMachine.State = TRSM_Done;
						break;
					}

					if (TrData->TrStateMachine.Done >= TrData->TrStateMachine.Length)
					{
						TrData->TrStateMachine.State = TRSM_Done;
						break;
					}
				}

				TrData->TrStateMachine.State = TRSM_Transferring;
				break;
			}
			else if (hcint.b.nak || hcint.b.frmovrun)
			{
				KdPrint((__FUNCTION__ "(%d, %d): Halted: NAK/FRMOVRUN - %08x\n", ADDRESS, ENDPOINT, hcint.d32));

				/*if (hcint.b.nak && TrData->TrStateMachine.Done > 0)
				{
					TrData->TrStateMachine.State = TRSM_Done;
					break;
				}*/

				/*if (TrData->EndpointHandle->Type == EndpointType_Control)
				{
					TrData->TrStateMachine.Buffer = (PCHAR)TrData->TrStateMachine.Buffer + TrData->TrStateMachine.Done;
					TrData->TrStateMachine.Length -= TrData->TrStateMachine.Done;

					TrData->TrStateMachine.State = TRSM_Init;
					if (!TrData->TrStateMachine.Done)
					{
						TrData->TrStateMachine.Strikes++;
					}

					ReviveTrSm(TrData);
					break;
				}*/

				/*if (hcint.b.nak)
				{
					if (TrData->StateMachine.Urb)
					{
						TrData->StateMachine.Urb->Hdr.Status = USBD_STATUS_TIMEOUT;
					}

					if (TrData->EndpointHandle->Type != EndpointType_Control && TrData->StateMachine.Urb)
					{
						INT in = TrData->StateMachine.Urb->TransferFlags & USBD_TRANSFER_DIRECTION_IN;

						if (in)
						{
							TrData->EndpointHandle->InToggle = TrData->TrStateMachine.Pid;
						}
						else
						{
							TrData->EndpointHandle->OutToggle = TrData->TrStateMachine.Pid;
						}
					}

					TrData->StateMachine.State = CHSM_Idle;


					controllerHandle->ChTtHubs[TrData->TrStateMachine.Channel] = 0;
					controllerHandle->ChTtPorts[TrData->TrStateMachine.Channel] = 0;

					controllerHandle->ChTrDatas[TrData->TrStateMachine.Channel] = NULL;
					KdPrint((__FUNCTION__ "(%d, %d): TRSM_TransferHalted: releasing channel %d\n", ADDRESS, ENDPOINT, TrData->TrStateMachine.Channel));
					TrData->EndpointHandle->UsbDeviceHandle->HostGlobalRegs->haintmsk &= ~(1 << TrData->TrStateMachine.Channel);
					Controller_SetChannelCallback(TrData->EndpointHandle->UsbDeviceHandle->UcxController, TrData->TrStateMachine.Channel, NULL, NULL);
					Controller_ReleaseChannel(TrData->EndpointHandle->UsbDeviceHandle->UcxController, TrData->TrStateMachine.Channel);
					TrData->StateMachine.Channel = TrData->TrStateMachine.Channel = -1;
					WdfRequestComplete(TrData->StateMachine.Request, STATUS_TIMEOUT);

					return STATUS_TIMEOUT;
				}*/

				TrData->TrStateMachine.State = TRSM_Init;
				regs->hcint = 0x3FFF;

				ReviveTrSm(TrData);

				controllerHandle = ControllerGetData(TrData->EndpointHandle->UsbDeviceHandle->UcxController);
				TrData->EndpointHandle->EpResumeContext = TrData;

				controllerHandle->ChTtHubs[TrData->TrStateMachine.Channel] = 0;
				controllerHandle->ChTtPorts[TrData->TrStateMachine.Channel] = 0;

				controllerHandle->ChTrDatas[TrData->TrStateMachine.Channel] = NULL;
				KdPrint((__FUNCTION__ "(%d, %d): TRSM_TransferHalted: releasing channel %d\n", ADDRESS, ENDPOINT, TrData->TrStateMachine.Channel));
				TrData->EndpointHandle->UsbDeviceHandle->HostGlobalRegs->haintmsk &= ~(1 << TrData->TrStateMachine.Channel);
				Controller_SetChannelCallback(TrData->EndpointHandle->UsbDeviceHandle->UcxController, TrData->TrStateMachine.Channel, NULL, NULL);
				Controller_ReleaseChannel(TrData->EndpointHandle->UsbDeviceHandle->UcxController, TrData->StateMachine.Channel);
				TrData->StateMachine.Channel = TrData->TrStateMachine.Channel = -1;

				ExSetTimer(
					TrData->EndpointHandle->EpResumeTimer,
					WDF_REL_TIMEOUT_IN_US(TR_PollingInterval(TrData)),
					0,
					NULL
				);

				//TrData->NextStateMachine = TrData->StateMachine;

				//Controller_InvokeTrSm(TrData->EndpointHandle->UsbDeviceHandle->UcxController, TrData);

				//break;
				return STATUS_PENDING;
			}
			else if (hcint.b.stall)
			{
				if (TrData->StateMachine.Urb)
				{
					TrData->StateMachine.Urb->UrbHeader.Status = USBD_STATUS_STALL_PID;
				}

				KdPrint((__FUNCTION__ "(%d, %d): Halted: stall - int %08x siz %08x char %08x splt %08x\n",
					ADDRESS, ENDPOINT,
					hcint.d32,
					hctsiz.d32,
					TrData->EndpointHandle->UsbDeviceHandle->ChannelRegs[TrData->StateMachine.Channel]->hcchar,
					TrData->EndpointHandle->UsbDeviceHandle->ChannelRegs[TrData->StateMachine.Channel]->hcsplt));

				TrData->StateMachine.State = CHSM_Idle;

				controllerHandle = ControllerGetData(TrData->EndpointHandle->UsbDeviceHandle->UcxController);
				controllerHandle->ChTtHubs[channel] = 0;
				controllerHandle->ChTtPorts[channel] = 0;

				controllerHandle->ChTrDatas[channel] = NULL;

				KdPrint((__FUNCTION__ "(%d, %d): TRSM_TransferHalted: releasing channel %d\n", ADDRESS, ENDPOINT, TrData->TrStateMachine.Channel));
				TrData->EndpointHandle->UsbDeviceHandle->HostGlobalRegs->haintmsk &= ~(1 << TrData->TrStateMachine.Channel);
				Controller_SetChannelCallback(TrData->EndpointHandle->UsbDeviceHandle->UcxController, TrData->TrStateMachine.Channel, NULL, NULL);
				Controller_ReleaseChannel(TrData->EndpointHandle->UsbDeviceHandle->UcxController, TrData->StateMachine.Channel);
				TrData->StateMachine.Channel = TrData->TrStateMachine.Channel = -1;
				WdfRequestComplete(TrData->StateMachine.Request, STATUS_UNSUCCESSFUL);

				return STATUS_UNSUCCESSFUL;
			}
			else
			{
				if (TrData->StateMachine.Urb)
				{
					TrData->StateMachine.Urb->UrbHeader.Status = USBD_STATUS_XACT_ERROR;
				}

				KdPrint((__FUNCTION__ "(%d, %d): Halted: unknown error - %08x\n", ADDRESS, ENDPOINT, hcint.d32));

				TrData->StateMachine.State = CHSM_Idle;

				controllerHandle = ControllerGetData(TrData->EndpointHandle->UsbDeviceHandle->UcxController);
				controllerHandle->ChTtHubs[channel] = 0;
				controllerHandle->ChTtPorts[channel] = 0;

				controllerHandle->ChTrDatas[channel] = NULL;

				KdPrint((__FUNCTION__ "(%d, %d): TRSM_TransferHalted: releasing channel %d\n", ADDRESS, ENDPOINT, TrData->TrStateMachine.Channel));
				TrData->EndpointHandle->UsbDeviceHandle->HostGlobalRegs->haintmsk &= ~(1 << TrData->TrStateMachine.Channel);
				Controller_SetChannelCallback(TrData->EndpointHandle->UsbDeviceHandle->UcxController, TrData->TrStateMachine.Channel, NULL, NULL);
				Controller_ReleaseChannel(TrData->EndpointHandle->UsbDeviceHandle->UcxController, TrData->StateMachine.Channel);
				TrData->StateMachine.Channel = TrData->TrStateMachine.Channel = -1;
				WdfRequestComplete(TrData->StateMachine.Request, STATUS_UNSUCCESSFUL);

				return STATUS_UNSUCCESSFUL;
			}

			break;
		}
		case TRSM_Done:
		{
			KdPrint((__FUNCTION__ "(%d, %d): TRSM_Done\n", ADDRESS, ENDPOINT));

			int channel = TrData->TrStateMachine.Channel;
			dwc_otg_hc_regs_t* regs = TrData->EndpointHandle->UsbDeviceHandle->ChannelRegs[channel];

			regs->hcint = 0xFFFFFFFF;
			regs->hcintmsk = 0;

			controllerHandle = ControllerGetData(TrData->EndpointHandle->UsbDeviceHandle->UcxController);

			ReviveTrSm(TrData);

			controllerHandle->ChTrDatas[channel] = NULL;
			controllerHandle->ChTtHubs[channel] = 0;
			controllerHandle->ChTtPorts[channel] = 0;

			KdPrint((__FUNCTION__ "(%d, %d): TRSM_Done: releasing channel %d\n", ADDRESS, ENDPOINT, TrData->TrStateMachine.Channel));
			TrData->EndpointHandle->UsbDeviceHandle->HostGlobalRegs->haintmsk &= ~(1 << TrData->TrStateMachine.Channel);
			Controller_SetChannelCallback(TrData->EndpointHandle->UsbDeviceHandle->UcxController, TrData->TrStateMachine.Channel, NULL, NULL);
			Controller_ReleaseChannel(TrData->EndpointHandle->UsbDeviceHandle->UcxController, TrData->StateMachine.Channel);
			TrData->StateMachine.Channel = TrData->TrStateMachine.Channel = -1;

			return STATUS_SUCCESS;
		}
		}
	}
}

VOID
Controller_RunCHSM(
	PVOID Context
)
{
	PTR_DATA TrData = (PTR_DATA)Context;
	KdPrint((__FUNCTION__ ": Invoking SM for endpoint (%d, %d) from interrupt\n", ADDRESS, ENDPOINT));

	TR_RunChSm(TrData, 1);
}

VOID
Endpoint_WdfEvtIoDeviceControl(
	WDFQUEUE      WdfQueue,
	WDFREQUEST    WdfRequest,
	size_t OutputBufferLength,
	size_t InputBufferLength,
	ULONG IoControlCode
)
/*++

Routine Description:

This is the WDF callback that delivers a WDFREQUEST from client. Since the
Control TransferRing Queue is configured as sequential, this callback can
only arrive when the number of driver owned requests is 0.

This routine will start mapping the request unless MappingState is Stopped.
If MappingState is Stopped, request pointer is saved, and it will be mapped
when the StartMapping callback arrives from ESM.

--*/
{
	PTR_DATA trData;
	BOOLEAN                 processTransfer;
	PURB           urb;
	WDF_REQUEST_PARAMETERS  wdfRequestParams;

	UNREFERENCED_PARAMETER((OutputBufferLength, InputBufferLength));

	if (IoControlCode != IOCTL_INTERNAL_USB_SUBMIT_URB) {
		KdPrint((__FUNCTION__ ": Unexpected IOCTL %d\n", IoControlCode));
		WdfRequestComplete(WdfRequest, STATUS_NOT_IMPLEMENTED);
		return;
	}

	processTransfer = FALSE;

	WDF_REQUEST_PARAMETERS_INIT(&wdfRequestParams);
	WdfRequestGetParameters(WdfRequest, &wdfRequestParams);

	urb = (PURB)wdfRequestParams.Parameters.Others.Arg1;

	KdPrint((__FUNCTION__ ": Processing URB with function 0x%04x\n", urb->UrbHeader.Function));

	trData = GetTRData(WdfQueue);

	/*INT channel;
	NTSTATUS status = Controller_AllocateChannel(trData->EndpointHandle->UsbDeviceHandle->UcxController, &channel);

	if (!NT_SUCCESS(status))
	{
		WdfRequestComplete(WdfRequest, STATUS_UNSUCCESSFUL);
		return;
	}*/

	trData->NextStateMachine.Request = WdfRequest;
	trData->NextStateMachine.Urb = urb;
	trData->NextStateMachine.State = trData->EndpointHandle->Type == EndpointType_Control ? CHSM_ControlSetup : CHSM_InterruptOrBulkData;
	trData->NextStateMachine.Channel = -1;

	//Controller_SetChannelCallback(trData->EndpointHandle->UsbDeviceHandle->UcxController, channel, Controller_RunCHSM, trData);

	Controller_InvokeTrSm(trData->EndpointHandle->UsbDeviceHandle->UcxController, trData);

	//TR_RunChSm(trData);

	//WdfRequestComplete(WdfRequest, STATUS_NOT_IMPLEMENTED);
}


// Dummy callback to capture and report unsupported requests
VOID
Endpoint_WdfEvtIoDefault(
	WDFQUEUE      WdfQueue,
	WDFREQUEST    WdfRequest
)
{
	UNREFERENCED_PARAMETER((WdfQueue));
	KdPrint((__FUNCTION__ ": Unexpected non-IOCTL request\n"));
	WdfRequestComplete(WdfRequest, STATUS_NOT_IMPLEMENTED);
	return;
}

NTSTATUS
Endpoint_CreateIoQueue(
	__in
	PENDPOINT_DATA				Endpoint
)
{
	KdPrint((__FUNCTION__ ": entry\n"));

	WDF_OBJECT_ATTRIBUTES   wdfAttributes;
	WDF_IO_QUEUE_CONFIG     wdfIoQueueConfig;
	WDFQUEUE                wdfQueue;

	WDF_IO_QUEUE_CONFIG_INIT(&wdfIoQueueConfig, WdfIoQueueDispatchSequential);
	wdfIoQueueConfig.EvtIoDeviceControl = Endpoint_WdfEvtIoDeviceControl;
	wdfIoQueueConfig.EvtIoInternalDeviceControl = Endpoint_WdfEvtIoDeviceControl;
	wdfIoQueueConfig.EvtIoDefault = Endpoint_WdfEvtIoDefault;
	//wdfIoQueueConfig.PowerManaged = WdfFalse;

	WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&wdfAttributes, TR_DATA);

	//
	// This queue handles USB bus traffic of downstream USB devices. Upon USB devices exiting
	// D0, Usbhub3 and UCX guarantee cancellation of this traffic, therefore the queue doesn't
	// need an EvtIoStop callback.
	//
	NTSTATUS status = WdfIoQueueCreate(Controller_GetWdfDevice(Endpoint->UsbDeviceHandle->UcxController),
		&wdfIoQueueConfig,
		&wdfAttributes,
		&wdfQueue);

	if (NT_SUCCESS(status))
	{
		PTR_DATA trData = GetTRData(wdfQueue);
		trData->EndpointHandle = Endpoint;
		WdfSpinLockCreate(WDF_NO_OBJECT_ATTRIBUTES, &trData->SpinLock);

		Endpoint->IoQueue = wdfQueue;
	}

	return status;
}

VOID
Controller_ResumeCh(
	_In_ PEX_TIMER Timer,
	_In_ PVOID Context
);

__drv_requiresIRQL(PASSIVE_LEVEL)
NTSTATUS
Endpoint_Create(
	__in
	UCXCONTROLLER               UcxController,
	__in
	UCXUSBDEVICE                UcxUsbDevice,
	__in
	PUCXENDPOINT_INIT           UcxEndpointInit,
	__in
	PUSB_ENDPOINT_DESCRIPTOR    UsbEndpointDescriptor,
	__out
	UCXENDPOINT*				Endpoint)
{
	NTSTATUS                    status;
	WDF_OBJECT_ATTRIBUTES       wdfAttributes;
	UCXENDPOINT                 ucxEndpoint;
	PENDPOINT_DATA              endpointData;

	PAGED_CODE();

	UNREFERENCED_PARAMETER(UcxController);

	WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&wdfAttributes, ENDPOINT_DATA);

	KdPrint((__FUNCTION__ ": entry\n"));

	status = UcxEndpointCreate(UcxUsbDevice,
		&UcxEndpointInit,
		&wdfAttributes,
		&ucxEndpoint);

	if (NT_SUCCESS(status))
	{
		if (Endpoint)
		{
			*Endpoint = ucxEndpoint;
		}

		endpointData = GetEndpointData(ucxEndpoint);

		endpointData->UcxUsbDevice = UcxUsbDevice;
		endpointData->UsbDeviceHandle = GetUsbDeviceData(UcxUsbDevice);

		endpointData->InToggle = DWC_HCTSIZ_DATA0;
		endpointData->OutToggle = DWC_HCTSIZ_DATA0;

		endpointData->UsbEndpointDescriptor = *UsbEndpointDescriptor;

		endpointData->MaxPacketSize = UsbEndpointDescriptor->wMaxPacketSize;

		if ((USB_ENDPOINT_DIRECTION_IN(endpointData->UsbEndpointDescriptor.bEndpointAddress)))
		{
			endpointData->Direction = Direction_In;
		}
		else
		{
			endpointData->Direction = Direction_Out;
		}

		switch (endpointData->UsbEndpointDescriptor.bmAttributes & USB_ENDPOINT_TYPE_MASK) {

		case USB_ENDPOINT_TYPE_ISOCHRONOUS:
			endpointData->Type = EndpointType_Isoch;
			break;

		case USB_ENDPOINT_TYPE_BULK:
			endpointData->Type = EndpointType_Bulk;
			break;

		case USB_ENDPOINT_TYPE_INTERRUPT:
			endpointData->Type = EndpointType_Interrupt;
			break;

		case USB_ENDPOINT_TYPE_CONTROL:
			endpointData->Type = EndpointType_Control;
			break;
		}

		if (endpointData->Type == EndpointType_Isoch)
		{
			KdPrint(("isochronous transfers not implemented\n"));

			return STATUS_NOT_IMPLEMENTED;
		}

		endpointData->EpResumeTimer = ExAllocateTimer(Controller_ResumeCh, &endpointData->EpResumeContext, EX_TIMER_HIGH_RESOLUTION);

		status = Endpoint_CreateIoQueue(endpointData);

		if (NT_SUCCESS(status))
		{
			UcxEndpointSetWdfIoQueue(ucxEndpoint, endpointData->IoQueue);
		}
	}

	return status;
}

VOID
Endpoint_WdfEvtPurgeComplete(
	WDFQUEUE    WdfQueue,
	WDFCONTEXT  WdfContext
)
{
	UCXENDPOINT ucxEndpoint = WdfContext;

	KdPrint((__FUNCTION__ ": entry\n"));

	UNREFERENCED_PARAMETER(WdfQueue);

	UcxEndpointPurgeComplete(ucxEndpoint);
}

VOID
Endpoint_WdfEvtAbortComplete(
	WDFQUEUE    WdfQueue,
	WDFCONTEXT  WdfContext
)
{
	UCXENDPOINT ucxEndpoint = WdfContext;

	KdPrint((__FUNCTION__ ": entry\n"));

	UNREFERENCED_PARAMETER(WdfQueue);

	UcxEndpointAbortComplete(ucxEndpoint);
}

VOID
Endpoint_UcxEvtEndpointStart(
	UCXCONTROLLER   UcxController,
	UCXENDPOINT     UcxEndpoint
)
{
	PENDPOINT_DATA          endpointData;

	KdPrint((__FUNCTION__ ": entry\n"));

	UNREFERENCED_PARAMETER(UcxController);

	endpointData = GetEndpointData(UcxEndpoint);

	WdfIoQueueStart(endpointData->IoQueue);
}

VOID
Endpoint_UcxEvtEndpointAbort(
	UCXCONTROLLER   UcxController,
	UCXENDPOINT     UcxEndpoint
)
{
	PENDPOINT_DATA          endpointData;

	KdPrint((__FUNCTION__ ": entry\n"));

	UNREFERENCED_PARAMETER(UcxController);

	endpointData = GetEndpointData(UcxEndpoint);

	WdfIoQueueStopAndPurge(endpointData->IoQueue, Endpoint_WdfEvtAbortComplete, UcxEndpoint);
}

VOID
Endpoint_UcxEvtEndpointPurge(
	UCXCONTROLLER   UcxController,
	UCXENDPOINT     UcxEndpoint
)
{
	PENDPOINT_DATA          endpointData;

	KdPrint((__FUNCTION__ ": entry\n"));

	UNREFERENCED_PARAMETER(UcxController);

	endpointData = GetEndpointData(UcxEndpoint);

	WdfIoQueuePurge(endpointData->IoQueue, Endpoint_WdfEvtPurgeComplete, UcxEndpoint);
}

VOID
Endpoint_UcxEvtEndpointReset(
	UCXCONTROLLER   UcxController,
	UCXENDPOINT     UcxEndpoint,
	WDFREQUEST      WdfRequest
)
{
	KdPrint((__FUNCTION__ ": entry\n"));

	UNREFERENCED_PARAMETER(UcxController);
	UNREFERENCED_PARAMETER(UcxEndpoint);
	UNREFERENCED_PARAMETER(WdfRequest);

	WdfRequestComplete(WdfRequest, STATUS_SUCCESS);
}

VOID
Endpoint_UcxEvtDefaultEndpointUpdate(
	UCXCONTROLLER   UcxController,
	WDFREQUEST      WdfRequest
)
{

	PENDPOINT_DATA              endpointData;
	PDEFAULT_ENDPOINT_UPDATE    defaultEndpointUpdate;
	WDF_REQUEST_PARAMETERS      wdfRequestParams;

	KdPrint((__FUNCTION__ ": entry\n"));

	UNREFERENCED_PARAMETER(UcxController);

	WDF_REQUEST_PARAMETERS_INIT(&wdfRequestParams);
	WdfRequestGetParameters(WdfRequest, &wdfRequestParams);

	defaultEndpointUpdate = (PDEFAULT_ENDPOINT_UPDATE)wdfRequestParams.Parameters.Others.Arg1;

	endpointData = GetEndpointData(defaultEndpointUpdate->DefaultEndpoint);

	endpointData->MaxPacketSize = defaultEndpointUpdate->MaxPacketSize;

	WdfRequestComplete(WdfRequest, STATUS_SUCCESS);
}

NTSTATUS
Endpoint_UcxEvtEndpointStaticStreamsAdd(
	UCXENDPOINT         UcxEndpoint,
	ULONG               NumberOfStreams,
	PUCXSSTREAMS_INIT   UcxStaticStreamsInit
)
{
	KdPrint((__FUNCTION__ ": entry\n"));

	UNREFERENCED_PARAMETER(UcxEndpoint);
	UNREFERENCED_PARAMETER(UcxStaticStreamsInit);
	UNREFERENCED_PARAMETER(NumberOfStreams);

	return STATUS_NOT_IMPLEMENTED;
}

VOID
Endpoint_UcxEvtEndpointStaticStreamsEnable(
	UCXENDPOINT UcxEndpoint,
	UCXSSTREAMS UcxStaticStreams,
	WDFREQUEST  WdfRequest
)
{
	KdPrint((__FUNCTION__ ": entry\n"));

	UNREFERENCED_PARAMETER(UcxEndpoint);
	UNREFERENCED_PARAMETER(UcxStaticStreams);
	UNREFERENCED_PARAMETER(WdfRequest);

	WdfRequestComplete(WdfRequest, STATUS_NOT_IMPLEMENTED);
}

VOID
Endpoint_UcxEvtEndpointStaticStreamsDisable(
	UCXENDPOINT UcxEndpoint,
	UCXSSTREAMS UcxStaticStreams,
	WDFREQUEST  WdfRequest
)
{
	KdPrint((__FUNCTION__ ": entry\n"));

	UNREFERENCED_PARAMETER(UcxEndpoint);
	UNREFERENCED_PARAMETER(UcxStaticStreams);
	UNREFERENCED_PARAMETER(WdfRequest);

	WdfRequestComplete(WdfRequest, STATUS_NOT_IMPLEMENTED);
}

NTSTATUS
Endpoint_UcxEvtEndpointEnableForwardProgress(
	UCXENDPOINT     UcxEndpoint,
	ULONG           MaximumTransferSize
)
{
	KdPrint((__FUNCTION__ ": entry\n"));

	UNREFERENCED_PARAMETER(UcxEndpoint);
	UNREFERENCED_PARAMETER(MaximumTransferSize);

	return STATUS_NOT_IMPLEMENTED;
}

VOID
Endpoint_UcxEvtEndpointOkToCancelTransfers(
	UCXENDPOINT     UcxEndpoint
)
{
	UNREFERENCED_PARAMETER(UcxEndpoint);
}

NTSTATUS
Endpoint_UcxEvtUsbDeviceDefaultEndpointAdd(
    UCXCONTROLLER       UcxController,
    UCXUSBDEVICE        UcxUsbDevice,
    ULONG               MaxPacketSize,
    PUCXENDPOINT_INIT   UcxEndpointInit
    )
/*++

Routine Description:

    This function creates the default endpoint.

--*/
{
    NTSTATUS                                status;
    UCX_DEFAULT_ENDPOINT_EVENT_CALLBACKS    ucxDefaultEndpointEventCallbacks;
    USB_ENDPOINT_DESCRIPTOR                 usbEndpointDescriptor;

    PAGED_CODE();

	KdPrint((__FUNCTION__ ": entry\n"));

    __try {

        //
        // The default control endpoint does not have a usb endpoint descriptor on
        // the device. This code creates its own usb endpoint descriptor describing
        // the default control endpoint. This is done to allow code reuse throughout
        // the endpoint object.
        //
        RtlZeroMemory(&usbEndpointDescriptor, sizeof(USB_ENDPOINT_DESCRIPTOR));
        usbEndpointDescriptor.bLength = sizeof(usbEndpointDescriptor);
        usbEndpointDescriptor.bDescriptorType = USB_ENDPOINT_DESCRIPTOR_TYPE;
        usbEndpointDescriptor.wMaxPacketSize = (USHORT)MaxPacketSize;
		usbEndpointDescriptor.bInterval = 1;

		if (GetUsbDeviceData(UcxUsbDevice)->UsbDeviceInfo.DeviceSpeed == UsbLowSpeed)
		{
			KdPrint(("making low-speed endpoint\n"));

			usbEndpointDescriptor.wMaxPacketSize = (USHORT)8;
		}

        UCX_DEFAULT_ENDPOINT_EVENT_CALLBACKS_INIT(&ucxDefaultEndpointEventCallbacks,
                                                  Endpoint_UcxEvtEndpointPurge,
                                                  Endpoint_UcxEvtEndpointStart,
                                                  Endpoint_UcxEvtEndpointAbort,
                                                  Endpoint_UcxEvtEndpointOkToCancelTransfers,
                                                  Endpoint_UcxEvtDefaultEndpointUpdate);

        UcxDefaultEndpointInitSetEventCallbacks(UcxEndpointInit, &ucxDefaultEndpointEventCallbacks);

        status = Endpoint_Create(UcxController,
                                 UcxUsbDevice,
                                 UcxEndpointInit,
                                 &usbEndpointDescriptor,
								 &(GetUsbDeviceData(UcxUsbDevice)->DefaultEndpoint));

    } __finally {

    }

    return status;
}

NTSTATUS
Endpoint_UcxEvtUsbDeviceEndpointAdd(
    UCXCONTROLLER                                               UcxController,
    UCXUSBDEVICE                                                UcxUsbDevice,
    PUSB_ENDPOINT_DESCRIPTOR                                    UsbEndpointDescriptor,
    ULONG                                                       UsbEndpointDescriptorBufferLength,
    PUSB_SUPERSPEED_ENDPOINT_COMPANION_DESCRIPTOR               SuperSpeedEndpointCompanionDescriptor,
    PUCXENDPOINT_INIT                                           UcxEndpointInit
    )
/*++

Routine Description:

    This function creates an endpoint.

--*/
{
    NTSTATUS                                                 status;
    UCX_ENDPOINT_EVENT_CALLBACKS                             ucxEndpointEventCallbacks;

    UNREFERENCED_PARAMETER(UsbEndpointDescriptorBufferLength);
	UNREFERENCED_PARAMETER(SuperSpeedEndpointCompanionDescriptor);

    PAGED_CODE();

	KdPrint((__FUNCTION__ ": entry\n"));

    __try {

        UCX_ENDPOINT_EVENT_CALLBACKS_INIT(&ucxEndpointEventCallbacks,
                                          Endpoint_UcxEvtEndpointPurge,
                                          Endpoint_UcxEvtEndpointStart,
                                          Endpoint_UcxEvtEndpointAbort,
                                          Endpoint_UcxEvtEndpointReset,
                                          Endpoint_UcxEvtEndpointOkToCancelTransfers,
                                          Endpoint_UcxEvtEndpointStaticStreamsAdd,
                                          Endpoint_UcxEvtEndpointStaticStreamsEnable,
                                          Endpoint_UcxEvtEndpointStaticStreamsDisable);

        UcxEndpointInitSetEventCallbacks(UcxEndpointInit, &ucxEndpointEventCallbacks);

        status = Endpoint_Create(UcxController,
                                 UcxUsbDevice,
                                 UcxEndpointInit,
                                 UsbEndpointDescriptor,
								 NULL);

    } __finally {

    }

    return status;
}


VOID
UsbDevice_UcxEvtHubInfo(
	UCXCONTROLLER   UcxController,
	WDFREQUEST      WdfRequest
)
{
	KdPrint((__FUNCTION__ ": entry\n"));

	WDF_REQUEST_PARAMETERS  wdfRequestParams;
	PUSBDEVICE_HUB_INFO     hubInfo;

	UNREFERENCED_PARAMETER(UcxController);

	WDF_REQUEST_PARAMETERS_INIT(&wdfRequestParams);
	WdfRequestGetParameters(WdfRequest, &wdfRequestParams);

	hubInfo = (PUSBDEVICE_HUB_INFO)wdfRequestParams.Parameters.Others.Arg1;

	WdfRequestComplete(WdfRequest, STATUS_SUCCESS);
}

VOID
UsbDevice_UcxEvtUpdate(
	UCXCONTROLLER   UcxController,
	WDFREQUEST      WdfRequest
)
{
	KdPrint((__FUNCTION__ ": entry\n"));

	WDF_REQUEST_PARAMETERS  wdfRequestParams;
	PUSBDEVICE_UPDATE usbDeviceUpdate;

	UNREFERENCED_PARAMETER(UcxController);

	WDF_REQUEST_PARAMETERS_INIT(&wdfRequestParams);
	WdfRequestGetParameters(WdfRequest, &wdfRequestParams);

	usbDeviceUpdate = (PUSBDEVICE_UPDATE)wdfRequestParams.Parameters.Others.Arg1;

	

	WdfRequestComplete(WdfRequest, STATUS_SUCCESS);
}

VOID
UsbDevice_UcxEvtReset(
	UCXCONTROLLER   UcxController,
	WDFREQUEST      WdfRequest
)
{
	KdPrint((__FUNCTION__ ": entry\n"));

	WDF_REQUEST_PARAMETERS  wdfRequestParams;
	PUSBDEVICE_RESET usbDeviceReset;

	UNREFERENCED_PARAMETER(UcxController);

	WDF_REQUEST_PARAMETERS_INIT(&wdfRequestParams);
	WdfRequestGetParameters(WdfRequest, &wdfRequestParams);

	usbDeviceReset = (PUSBDEVICE_RESET)wdfRequestParams.Parameters.Others.Arg1;

	PUSBDEVICE_DATA usbDeviceData = GetUsbDeviceData(usbDeviceReset->UsbDevice);

	UNREFERENCED_PARAMETER(usbDeviceData);

	WdfRequestComplete(WdfRequest, STATUS_SUCCESS);
}

VOID
UsbDevice_UcxEvtEnable(
	UCXCONTROLLER   UcxController,
	WDFREQUEST      WdfRequest
)
{
	KdPrint((__FUNCTION__ ": entry\n"));

	WDF_REQUEST_PARAMETERS  wdfRequestParams;
	PUSBDEVICE_ENABLE usbDeviceEnable;

	UNREFERENCED_PARAMETER(UcxController);

	WDF_REQUEST_PARAMETERS_INIT(&wdfRequestParams);
	WdfRequestGetParameters(WdfRequest, &wdfRequestParams);

	usbDeviceEnable = (PUSBDEVICE_ENABLE)wdfRequestParams.Parameters.Others.Arg1;

	PUSBDEVICE_DATA usbDeviceData = GetUsbDeviceData(usbDeviceEnable->UsbDevice);

	UNREFERENCED_PARAMETER(usbDeviceData);

	WdfRequestComplete(WdfRequest, STATUS_SUCCESS);
}

VOID
UsbDevice_UcxEvtDisable(
	UCXCONTROLLER   UcxController,
	WDFREQUEST      WdfRequest
)
{
	KdPrint((__FUNCTION__ ": entry\n"));

	WDF_REQUEST_PARAMETERS  wdfRequestParams;
	PUSBDEVICE_DISABLE usbDeviceDisable;

	UNREFERENCED_PARAMETER(UcxController);

	WDF_REQUEST_PARAMETERS_INIT(&wdfRequestParams);
	WdfRequestGetParameters(WdfRequest, &wdfRequestParams);

	usbDeviceDisable = (PUSBDEVICE_DISABLE)wdfRequestParams.Parameters.Others.Arg1;

	PUSBDEVICE_DATA usbDeviceData = GetUsbDeviceData(usbDeviceDisable->UsbDevice);

	UNREFERENCED_PARAMETER(usbDeviceData);

	WdfRequestComplete(WdfRequest, STATUS_SUCCESS);
}

VOID
UsbDevice_UcxEvtEndpointsConfigure(
	UCXCONTROLLER   UcxController,
	WDFREQUEST      WdfRequest
)
{
	KdPrint((__FUNCTION__ ": entry\n"));

	WDF_REQUEST_PARAMETERS  wdfRequestParams;
	PENDPOINTS_CONFIGURE endpointsConfigure;

	UNREFERENCED_PARAMETER(UcxController);

	WDF_REQUEST_PARAMETERS_INIT(&wdfRequestParams);
	WdfRequestGetParameters(WdfRequest, &wdfRequestParams);

	endpointsConfigure = (PENDPOINTS_CONFIGURE)wdfRequestParams.Parameters.Others.Arg1;

	PUSBDEVICE_DATA usbDeviceData = GetUsbDeviceData(endpointsConfigure->UsbDevice);

	UNREFERENCED_PARAMETER(usbDeviceData);

	WdfRequestComplete(WdfRequest, STATUS_SUCCESS);
}

NTSTATUS
USBPORT_AllocateUsbAddress(
	UCXCONTROLLER UcxController,
	PUSHORT AssignedAddress
)
/*++

Routine Description:

Allocates a USB address from a bitmap of availble addresses. Valid USB address (1..127) to use for a device.
returns 0 and STATUS_INSUFFICIENT_RESOURCES if no device address available.

--*/
{
	PCONTROLLER_DATA controllerData;
	USHORT address;
	NTSTATUS nts;
	PUSB_ADDRESS_LIST usbAddressList;
	ULONG bit;

	controllerData = ControllerGetData(UcxController);

	do {
		nts = STATUS_INSUFFICIENT_RESOURCES;
		address = 0;

		if (controllerData->UsbAddressInit == 0) {
			usbAddressList = &controllerData->UsbAddressList;

			if (usbAddressList) {
				RtlInitializeBitMap(&usbAddressList->Bitmap,
					&usbAddressList->Bits[0],
					USB_MAX_ADDRESS_COUNT);

				RtlClearAllBits(&usbAddressList->Bitmap);
				// reserve bit 0 (default address)
				RtlSetBit(&usbAddressList->Bitmap, 0);

				controllerData->UsbAddressInit = 1;
			}

		}
		else {
			usbAddressList = &controllerData->UsbAddressList;
		}

		if (usbAddressList == NULL) {
			break;
		}

		bit = RtlFindClearBits(&usbAddressList->Bitmap, 1, 1);

		// if in range assign address
		if ((bit != 0) && (bit < USB_MAX_ADDRESS_COUNT)) {
			address = (USHORT)bit;
			RtlSetBit(&usbAddressList->Bitmap, bit);
			nts = STATUS_SUCCESS;
		}

	}while(0);

#if DBG
	if (address == 0 && usbAddressList) {
		// no free addresses?
		NT_ASSERTMSG("No free addresses", FALSE);
	}
#endif

	KdPrint(("'USBPORT assigning Address %d\n", address));

	*AssignedAddress = address;

	return nts;
}

VOID
RunSmDpc(
	_In_     struct _KDPC *Dpc,
	_In_opt_ PVOID        DeferredContext,
	_In_opt_ PVOID        SystemArgument1,
	_In_opt_ PVOID        SystemArgument2
)
{
	UNREFERENCED_PARAMETER(Dpc);
	UNREFERENCED_PARAMETER(DeferredContext);
	UNREFERENCED_PARAMETER(SystemArgument2);

	PTR_DATA TrData = (PTR_DATA)SystemArgument1;

	KdPrint((__FUNCTION__ "(%d, %d): last state %d, next state %d\n", ADDRESS, ENDPOINT, TrData->StateMachine.State, TrData->NextStateMachine.State));
	TrData->StateMachine = TrData->NextStateMachine;

	TR_RunChSm(TrData, 0);
}

VOID
Controller_InvokeTrSm(
	_In_ UCXCONTROLLER UcxController,
	_In_ PTR_DATA TrData
)
{
	UNREFERENCED_PARAMETER(UcxController);

	if (!TrData->EndpointHandle->EpSmDpcInited)
	{
		KeInitializeDpc(&TrData->EndpointHandle->EpSmDpc, RunSmDpc, NULL);

		TrData->EndpointHandle->EpSmDpcInited = TRUE;
	}

	KeInsertQueueDpc(&TrData->EndpointHandle->EpSmDpc, TrData, NULL);
}

VOID
Controller_ResumeCh(
	_In_ PEX_TIMER Timer,
	_In_ PVOID Context
)
{
	UNREFERENCED_PARAMETER(Timer);

	PTR_DATA TrData = *(PTR_DATA*)Context;

	KdPrint((__FUNCTION__ ": Invoking SM for endpoint (%d, %d) from timer\n", ADDRESS, ENDPOINT));

	TrData->NextStateMachine = TrData->StateMachine;
	Controller_InvokeTrSm(TrData->EndpointHandle->UsbDeviceHandle->UcxController, TrData);
}

VOID
UsbDevice_UcxEvtAddress(
	UCXCONTROLLER   UcxController,
	WDFREQUEST      WdfRequest
)
{
	KdPrint((__FUNCTION__ ": entry\n"));

	WDF_REQUEST_PARAMETERS  wdfRequestParams;
	PUSBDEVICE_ADDRESS usbDeviceAddress;

	UNREFERENCED_PARAMETER(UcxController);

	WDF_REQUEST_PARAMETERS_INIT(&wdfRequestParams);
	WdfRequestGetParameters(WdfRequest, &wdfRequestParams);

	usbDeviceAddress = (PUSBDEVICE_ADDRESS)wdfRequestParams.Parameters.Others.Arg1;

	PUSBDEVICE_DATA usbDeviceData = GetUsbDeviceData(usbDeviceAddress->UsbDevice);

	PENDPOINT_DATA endpointData = GetEndpointData(usbDeviceData->DefaultEndpoint);

	USHORT address;
	if (!NT_SUCCESS(USBPORT_AllocateUsbAddress(UcxController, &address)))
	{
		WdfRequestComplete(WdfRequest, STATUS_UNSUCCESSFUL);
		return;
	}

	/*INT channel;
	NTSTATUS status = Controller_AllocateChannel(endpointData->UsbDeviceHandle->UcxController, &channel);

	if (!NT_SUCCESS(status))
	{
		WdfRequestComplete(WdfRequest, STATUS_UNSUCCESSFUL);
		return;
	}*/

	PTR_DATA trData = GetTRData(endpointData->IoQueue);
	trData->NextStateMachine.Request = WdfRequest;
	trData->NextStateMachine.State = CHSM_AddressSetup;
	trData->NextStateMachine.Address = address;
	trData->NextStateMachine.Channel = -1;
	
	usbDeviceAddress->Address = address;

	//Controller_SetChannelCallback(trData->EndpointHandle->UsbDeviceHandle->UcxController, channel, Controller_RunCHSM, trData);

	Controller_InvokeTrSm(trData->EndpointHandle->UsbDeviceHandle->UcxController, trData);

	//WdfRequestComplete(WdfRequest, STATUS_SUCCESS);

}

NTSTATUS
UsbDevice_UcxEvtDeviceAdd(
	UCXCONTROLLER       UcxController,
	PUCXUSBDEVICE_INFO  UsbDeviceInfo,
	PUCXUSBDEVICE_INIT  UsbDeviceInit
)
{
	// TODO: implement
	KdPrint((__FUNCTION__ ": entry\n"));

	NTSTATUS						status;
	UCX_USBDEVICE_EVENT_CALLBACKS   ucxUsbDeviceEventCallbacks;
	WDF_OBJECT_ATTRIBUTES           wdfAttributes;
	UCXUSBDEVICE                    ucxUsbDevice;
	PUSBDEVICE_DATA                 usbDeviceData;
	PCONTROLLER_DATA					controllerCtx;
	PDEVICE_CONTEXT					deviceCtx;

	UNREFERENCED_PARAMETER(UcxController);
	UNREFERENCED_PARAMETER(UsbDeviceInfo);
	UNREFERENCED_PARAMETER(UsbDeviceInit);

	controllerCtx = ControllerGetData(UcxController);
	deviceCtx = DeviceGetContext(controllerCtx->WdfDevice);

	UCX_USBDEVICE_EVENT_CALLBACKS_INIT(&ucxUsbDeviceEventCallbacks,
		UsbDevice_UcxEvtEndpointsConfigure,
		UsbDevice_UcxEvtEnable,
		UsbDevice_UcxEvtDisable,
		UsbDevice_UcxEvtReset,
		UsbDevice_UcxEvtAddress,
		UsbDevice_UcxEvtUpdate,
		UsbDevice_UcxEvtHubInfo,
		Endpoint_UcxEvtUsbDeviceDefaultEndpointAdd,
		Endpoint_UcxEvtUsbDeviceEndpointAdd);

	UcxUsbDeviceInitSetEventCallbacks(UsbDeviceInit, &ucxUsbDeviceEventCallbacks);

	//
	// Create the UCX USB device object.
	//
	WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&wdfAttributes, USBDEVICE_DATA);

	status = UcxUsbDeviceCreate(UcxController,
		&UsbDeviceInit,
		&wdfAttributes,
		&ucxUsbDevice);

	if (NT_SUCCESS(status))
	{
		usbDeviceData = GetUsbDeviceData(ucxUsbDevice);

		usbDeviceData->UcxUsbDevice = ucxUsbDevice;
		usbDeviceData->UcxController = UcxController;
		usbDeviceData->UsbDeviceInfo = *UsbDeviceInfo;

		LARGE_INTEGER hostBase;
		hostBase.QuadPart = deviceCtx->MemoryBase.QuadPart + 0x400;

		usbDeviceData->HostGlobalRegs = MmMapIoSpace(hostBase, sizeof(dwc_otg_host_global_regs_t), MmNonCached);
		
		LARGE_INTEGER channelBase;
		channelBase.QuadPart = deviceCtx->MemoryBase.QuadPart + DWC_OTG_HOST_CHAN_REGS_OFFSET;

		for (int i = 0; i < NUM_CHANNELS; i++)
		{
			usbDeviceData->ChannelRegs[i] = MmMapIoSpace(channelBase, sizeof(dwc_otg_hc_regs_t), MmNonCached);
			channelBase.QuadPart += DWC_OTG_CHAN_REGS_OFFSET;
		}
	}

	return status;
}