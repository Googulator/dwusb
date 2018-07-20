/*++

Module Name:

    device.c - Device handling events for example driver.

Abstract:

   This file contains the device entry points and callbacks.
    
Environment:

    Kernel-mode Driver Framework

--*/

#include "driver.h"
#include "device.tmh"

// FIXME don't hardcode these! use resources!
//#define DWUSB_BASE 0x3F980000
//#define DWUSB_INT 0x29

#ifdef ALLOC_PRAGMA
#pragma alloc_text (PAGE, dwusbCreateDevice)
#pragma alloc_text (PAGE, Controller_GetWdfDevice)
#endif

typedef struct _ROOT_HUB_DATA {
	dwc_otg_core_global_regs_t* CoreGlobalRegs;
	dwc_otg_host_global_regs_t* HostGlobalRegs;
	volatile uint32_t* Hprt0;

	PEX_TIMER ExTimerResetComplete;
	PEX_TIMER ExTimerResumeComplete;

	PEX_TIMER ExTimerResetSafeComplete;

	UCXROOTHUB UcxRootHub;

	BOOLEAN ResetState;
} ROOTHUB_DATA, *PROOTHUB_DATA;

typedef struct _REQUEST_DATA {
	int Unused;
} REQUEST_DATA, *PREQUEST_DATA;

typedef struct _INTERRUPT_CONTEXT {
	PCONTROLLER_DATA ControllerHandle;
} INTERRUPT_CONTEXT, *PINTERRUPT_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(ROOTHUB_DATA, RootHubGetData)
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(REQUEST_DATA, RequestGetData)
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(INTERRUPT_CONTEXT, InterruptGetData)

WDFDEVICE
Controller_GetWdfDevice(
	_In_ UCXCONTROLLER UcxController
)
{
	return ControllerGetData(UcxController)->WdfDevice;
}

VOID
Controller_SetChannelCallback(
	_In_ UCXCONTROLLER UcxController,
	_In_ int Channel,
	_In_ PFN_CHANNEL_CALLBACK Callback,
	_In_opt_ PVOID Context
)
{
	PCONTROLLER_DATA data = ControllerGetData(UcxController);

	if (Channel >= 0 && Channel < 16)
	{
		data->ChannelCallbacks[Channel] = Callback;
		data->ChannelCallbackContext[Channel] = Context;
	}
}

VOID
RootHub_UcxEvtGetInfo(
	UCXROOTHUB  UcxRootHub,
	WDFREQUEST  WdfRequest
)
/*++

Routine Description:

This routine is called by UCX to process an
IOCTL_INTERNAL_USB_GET_ROOTHUB_INFO request and return information
about the root hub in the ROOTHUB_INFO structure attached to the
request.

The main information of interest returned in the ROOTHUB_INFO
structure is the number of 2.0 ports and the number of 3.0 ports
supported by this root hub.  That information was collected by
RootHub_PrepareHardware() when it processed the xHCI Supported
Protocol Capability list.

Return Value:

None directly from this routine.

The request completion status in the WdfRequest.

--*/
{
	PROOTHUB_DATA           rootHubData;
	WDF_REQUEST_PARAMETERS  wdfRequestParams;
	PROOTHUB_INFO           roothubInfo;
	NTSTATUS                status = STATUS_SUCCESS;

	KdPrint((__FUNCTION__ "\n"));

	__try {

		rootHubData = RootHubGetData(UcxRootHub);

		WDF_REQUEST_PARAMETERS_INIT(&wdfRequestParams);
		WdfRequestGetParameters(WdfRequest, &wdfRequestParams);

		roothubInfo = (PROOTHUB_INFO)wdfRequestParams.Parameters.Others.Arg1;

		if (roothubInfo->Size < sizeof(ROOTHUB_INFO)) {

			status = STATUS_INVALID_PARAMETER;
			return;
		}

		roothubInfo->ControllerType = ControllerTypeSoftXhci;
		roothubInfo->NumberOf20Ports = 1;
		roothubInfo->NumberOf30Ports = 0;
		//
		// Units here are in microseconds.
		//
		roothubInfo->MaxU1ExitLatency = 10000;
		roothubInfo->MaxU2ExitLatency = 10000;
		
		status = STATUS_SUCCESS;

	} __finally {

		WdfRequestComplete(WdfRequest, status);
	}

	return;
}

VOID
RootHub_UcxEvtGet20PortInfo(
	UCXROOTHUB  UcxRootHub,
	WDFREQUEST  WdfRequest
)
/*++

Routine Description:

This routine is called by UCX to process an
IOCTL_INTERNAL_USB_ROOTHUB_GET_20PORT_INFO request and return
information about the root hub in the ROOTHUB_20PORTS_INFO structure
attached to the request.

The main information of interest returned in the
ROOTHUB_20PORTS_INFO structure is the list of 2.0 ports supported by
this root hub.  That is returned in the PortInfoArray array of
ROOTHUB_20PORT_INFO structures.  The list of 2.0 ports supported by
this root hub was collected by RootHub_PrepareHardware() when it
processed the xHCI Supported Protocol Capability list.

Return Value:

None directly from this routine.

The request completion status in the WdfRequest.

--*/
{
	PROOTHUB_DATA           rootHubData;
	WDF_REQUEST_PARAMETERS  wdfRequestParams;
	PROOTHUB_20PORTS_INFO   roothub20PortsInfo;
	PROOTHUB_20PORT_INFO*   roothub20PortInfo;
	USHORT                  srcPortIndex;
	USHORT                  dstPortIndex;
	NTSTATUS                status = STATUS_SUCCESS;


	KdPrint((__FUNCTION__ "\n"));

	__try {

		rootHubData = RootHubGetData(UcxRootHub);

		WDF_REQUEST_PARAMETERS_INIT(&wdfRequestParams);
		WdfRequestGetParameters(WdfRequest, &wdfRequestParams);

		roothub20PortsInfo = (PROOTHUB_20PORTS_INFO)wdfRequestParams.Parameters.Others.Arg1;
		roothub20PortInfo = &roothub20PortsInfo->PortInfoArray[0];

		if (roothub20PortsInfo->Size < sizeof(ROOTHUB_20PORTS_INFO)) {

			status = STATUS_INVALID_PARAMETER;
			return;
		}

		if (roothub20PortsInfo->NumberOfPorts != 1) {
			status = STATUS_INVALID_PARAMETER;
			return;
		}

		if (roothub20PortsInfo->PortInfoSize < sizeof(ROOTHUB_20PORT_INFO)) {
			status = STATUS_INVALID_PARAMETER;
			return;
		}

		//
		// Return port information for all of the MajorRevision == 2
		// ports in the PortData[] array.
		//
		dstPortIndex = 0;

		for (srcPortIndex = 0;
			(srcPortIndex < 1 && dstPortIndex < roothub20PortsInfo->NumberOfPorts);
			srcPortIndex++) {

			//
			// The PortData[] array is 0-based indexed while
			// port numbers are 1-based indexed.
			//
			roothub20PortInfo[dstPortIndex]->PortNumber = srcPortIndex + 1;

			roothub20PortInfo[dstPortIndex]->Removable = TriStateFalse;
			roothub20PortInfo[dstPortIndex]->IntegratedHubImplemented = TriStateFalse;

			roothub20PortInfo[dstPortIndex]->MinorRevision = 0;
			roothub20PortInfo[dstPortIndex]->HubDepth = 4;

			dstPortIndex++;
		}

		status = STATUS_SUCCESS;

	} __finally {

		WdfRequestComplete(WdfRequest, status);
	}

	return;
}

VOID
RootHub_UcxEvtGet30PortInfo(
	UCXROOTHUB  UcxRootHub,
	WDFREQUEST  WdfRequest
)
/*++

Routine Description:

This routine is called by UCX to process an
IOCTL_INTERNAL_USB_ROOTHUB_GET_30PORT_INFO request and return
information about the root hub in the ROOTHUB_30PORTS_INFO structure
attached to the request.

The main information of interest returned in the
ROOTHUB_30PORTS_INFO structure is the list of 3.0 ports supported by
this root hub.  That is returned in the PortInfoArray array of
ROOTHUB_30PORT_INFO structures.  The list of 3.0 ports supported by
this root hub was collected by RootHub_PrepareHardware() when it
processed the xHCI Supported Protocol Capability list.

Return Value:

None directly from this routine.

The request completion status in the WdfRequest.

--*/
{
	PROOTHUB_DATA            rootHubData;
	WDF_REQUEST_PARAMETERS   wdfRequestParams;
	PROOTHUB_30PORTS_INFO    roothub30PortsInfo;
	PROOTHUB_30PORT_INFO_EX* roothub30PortInfo;
	USHORT                   dstPortIndex;
	NTSTATUS                 status = STATUS_SUCCESS;

	KdPrint((__FUNCTION__ "\n"));

	__try {

		rootHubData = RootHubGetData(UcxRootHub);

		WDF_REQUEST_PARAMETERS_INIT(&wdfRequestParams);
		WdfRequestGetParameters(WdfRequest, &wdfRequestParams);

		roothub30PortsInfo = (PROOTHUB_30PORTS_INFO)wdfRequestParams.Parameters.Others.Arg1;
		roothub30PortInfo = (PROOTHUB_30PORT_INFO_EX*)&roothub30PortsInfo->PortInfoArray[0];

		if (roothub30PortsInfo->Size < sizeof(ROOTHUB_30PORTS_INFO)) {

			status = STATUS_INVALID_PARAMETER;
			return;
		}

		if (roothub30PortsInfo->NumberOfPorts != 0) {
			status = STATUS_INVALID_PARAMETER;
			return;
		}

		if (roothub30PortsInfo->PortInfoSize < sizeof(ROOTHUB_30PORT_INFO)) {
			status = STATUS_INVALID_PARAMETER;
			return;
		}

		//
		// Return port information for all of the MajorRevision == 3
		// ports in the PortData[] array.
		//
		dstPortIndex = 0;

		status = STATUS_SUCCESS;

	} __finally {

		WdfRequestComplete(WdfRequest, status);
	}

	return;
}

VOID
RootHub_UcxEvtInterruptTransfer(
	__in
	UCXROOTHUB  UcxRootHub,
	__in
	WDFREQUEST  WdfRequest
)
{
	NTSTATUS                status = STATUS_SUCCESS;
	WDF_REQUEST_PARAMETERS  wdfRequestParams;
	PURB                    urb;
	ULONG                   transferBufferLength;
	PVOID                   transferBuffer;

	KdPrint((__FUNCTION__ "\n"));

	UNREFERENCED_PARAMETER(UcxRootHub);

	__try
	{
		WDF_REQUEST_PARAMETERS_INIT(&wdfRequestParams);
		WdfRequestGetParameters(WdfRequest, &wdfRequestParams);

		urb = (PURB)wdfRequestParams.Parameters.Others.Arg1;
		transferBuffer = urb->UrbBulkOrInterruptTransfer.TransferBuffer;
		transferBufferLength = urb->UrbBulkOrInterruptTransfer.TransferBufferLength;

		RtlZeroMemory(transferBuffer, transferBufferLength);

		PROOTHUB_DATA rootHubData = RootHubGetData(UcxRootHub);

		hprt0_data_t hprt0;

		KeMemoryBarrier();
		_DataSynchronizationBarrier();

		hprt0.d32 = READ_REGISTER_ULONG((volatile ULONG*)rootHubData->Hprt0);

		KeMemoryBarrier();
		_DataSynchronizationBarrier();

		if (hprt0.b.prtenchng || hprt0.b.prtovrcurrchng || hprt0.b.prtconndet/* || rootHubData->ResetState*/)
		{
			// port 1 changed
			((PUCHAR)transferBuffer)[0] |= 1 << 1;
		}
		
		status = STATUS_SUCCESS;
		urb->UrbHeader.Status = USBD_STATUS_SUCCESS;
	}
	__finally
	{
		WdfRequestComplete(WdfRequest, status);
	}
}

VOID
RootHub_UcxEvtClearHubFeature(
    UCXROOTHUB  UcxRootHub,
    WDFREQUEST  WdfRequest
    )
/*++

Routine Description:


Arguments:

    UcxRootHub - The UCX root hub object.

    WdfRequest - The WDF request object for the Clear Hub Feature request.

Return Value:

    None directly from this routine.

    The request completion status in the WdfRequest and the USBD_STATUS
    in the urb header.

--*/
{
    NTSTATUS                        status = STATUS_SUCCESS;
    PROOTHUB_DATA                   rootHubData;
    WDF_REQUEST_PARAMETERS          wdfRequestParams;
    PURB                            urb;
    PWDF_USB_CONTROL_SETUP_PACKET   setupPacket;
    ULONG                           featureSelector;

	KdPrint((__FUNCTION__ "\n"));

    UNREFERENCED_PARAMETER(UcxRootHub);

    __try {

        rootHubData = RootHubGetData(UcxRootHub);

        WDF_REQUEST_PARAMETERS_INIT(&wdfRequestParams);
        WdfRequestGetParameters(WdfRequest, &wdfRequestParams);

        urb = (PURB)wdfRequestParams.Parameters.Others.Arg1;
        setupPacket = (PWDF_USB_CONTROL_SETUP_PACKET)&urb->UrbControlTransferEx.SetupPacket[0];

        //
        // USB Specification 2.0, 11.24.2.1 Clear Hub Feature
        // USB Specification 3.0, 10.14.2.1 Clear Hub Feature
        //
        featureSelector = setupPacket->Packet.wValue.Value;

        if ((setupPacket->Packet.bm.Byte != 0x20) ||
            (setupPacket->Packet.bRequest != USB_REQUEST_CLEAR_FEATURE) ||
            (setupPacket->Packet.wIndex.Value != 0) ||
            (setupPacket->Packet.wLength != 0)) {
            urb->UrbHeader.Status = USBD_STATUS_STALL_PID;
            status = STATUS_UNSUCCESSFUL;
			return;
        }

        switch (featureSelector) {

        case C_HUB_LOCAL_POWER:
            urb->UrbHeader.Status = USBD_STATUS_SUCCESS;
            status = STATUS_SUCCESS;
            break;

        case C_HUB_OVER_CURRENT:
            urb->UrbHeader.Status = USBD_STATUS_SUCCESS;
            status = STATUS_SUCCESS;
            break;

        default:
            urb->UrbHeader.Status = USBD_STATUS_STALL_PID;
            status = STATUS_UNSUCCESSFUL;
			return;
        }

    } __finally {

        NT_ASSERT(status == STATUS_SUCCESS);
        WdfRequestComplete(WdfRequest, status);
    }

    return;
}

VOID
RootHub_UcxEvtClearPortFeature(
    UCXROOTHUB  UcxRootHub,
    WDFREQUEST  WdfRequest
    )
/*++

Routine Description:


Arguments:

    UcxRootHub - The UCX root hub object.

    WdfRequest - The WDF request object for the Clear Port Feature request.

Return Value:

    None directly from this routine.

    The request completion status in the WdfRequest and the USBD_STATUS
    in the urb header.

--*/
{
    NTSTATUS                        status = STATUS_SUCCESS;
    PROOTHUB_DATA                   rootHubData;
    WDF_REQUEST_PARAMETERS          wdfRequestParams;
    PURB                            urb;
    PWDF_USB_CONTROL_SETUP_PACKET   setupPacket;
    ULONG                           portNumber;
    ULONG                           featureSelector;
    ULONG                           featureSpecificValue;
	hprt0_data_t hprt0;

	KdPrint((__FUNCTION__ "\n"));

    __try {

        rootHubData = RootHubGetData(UcxRootHub);

        WDF_REQUEST_PARAMETERS_INIT(&wdfRequestParams);
        WdfRequestGetParameters(WdfRequest, &wdfRequestParams);

        urb = (PURB)wdfRequestParams.Parameters.Others.Arg1;
        setupPacket = (PWDF_USB_CONTROL_SETUP_PACKET)&urb->UrbControlTransferEx.SetupPacket[0];

        //
        // USB Specification 2.0, 11.24.2.2 Clear Port Feature
        // USB Specification 3.0, 10.14.2.2 Clear Port Feature
        //
        featureSelector      = setupPacket->Packet.wValue.Value;
        portNumber           = setupPacket->Packet.wIndex.Bytes.LowByte;
        featureSpecificValue = setupPacket->Packet.wIndex.Bytes.HiByte;

        if ((setupPacket->Packet.bm.Byte != 0x23) ||
            (setupPacket->Packet.bRequest != USB_REQUEST_CLEAR_FEATURE) ||
            (portNumber == 0) ||
            (portNumber > 1) ||
            (setupPacket->Packet.wLength != 0)) {

            urb->UrbHeader.Status = USBD_STATUS_STALL_PID;
            status = STATUS_UNSUCCESSFUL;
            return;
        }

        //
        // The most significant byte of wIndex is zero, except when
        // the feature selector is PORT_INDICATOR.
        //
        if ((featureSelector != PORT_INDICATOR) &&
            (featureSpecificValue != 0)) {
            urb->UrbHeader.Status = USBD_STATUS_STALL_PID;
            status = STATUS_UNSUCCESSFUL;
            return;
        }

        switch (featureSelector) {

        case PORT_ENABLE:
			
            //
            // Clearing the PORT_ENABLE feature causes the port
            // to be placed in the Disabled state.
            //
			hprt0.d32 = READ_REGISTER_ULONG((volatile ULONG*)rootHubData->Hprt0);
			hprt0.b.prtconndet = 0;
			hprt0.b.prtena = 0;
			hprt0.b.prtenchng = 0;
			hprt0.b.prtovrcurrchng = 0;
			//hprt0.b.prtena = 1;
			WRITE_REGISTER_ULONG((volatile ULONG*)rootHubData->Hprt0, (ULONG)hprt0.d32);

            urb->UrbHeader.Status = USBD_STATUS_SUCCESS;
            status = STATUS_SUCCESS;
            break;

        case PORT_SUSPEND:
			
			hprt0.d32 = READ_REGISTER_ULONG((volatile ULONG*)rootHubData->Hprt0);
			hprt0.b.prtconndet = 0;
			hprt0.b.prtena = 0;
			hprt0.b.prtenchng = 0;
			hprt0.b.prtovrcurrchng = 0;
			hprt0.b.prtres = 1;
			WRITE_REGISTER_ULONG((volatile ULONG*)rootHubData->Hprt0, (ULONG)hprt0.d32);

            //
            // Software shall ensure that resume is signaled for at
            // least 20 ms (TDRSMDN).  Software shall start timing
            // TDRSMDN from the write of '15' (Resume) to PLS.
            // After TDRSMDN is complete, software shall write a '0'
            // (U0) to the PLS field.
            //
            ExSetTimer(rootHubData->ExTimerResumeComplete,
                        WDF_REL_TIMEOUT_IN_MS(150),
                        0,
                        NULL);

            urb->UrbHeader.Status = USBD_STATUS_SUCCESS;
            status = STATUS_SUCCESS;
            break;

        case PORT_POWER:
			hprt0.d32 = READ_REGISTER_ULONG((volatile ULONG*)rootHubData->Hprt0);
			hprt0.b.prtconndet = 0;
			hprt0.b.prtena = 0;
			hprt0.b.prtenchng = 0;
			hprt0.b.prtovrcurrchng = 0;
			hprt0.b.prtpwr = 0;
			WRITE_REGISTER_ULONG((volatile ULONG*)rootHubData->Hprt0, (ULONG)hprt0.d32);

            urb->UrbHeader.Status = USBD_STATUS_SUCCESS;
            status = STATUS_SUCCESS;
            break;

        case PORT_INDICATOR:
            urb->UrbHeader.Status = USBD_STATUS_SUCCESS;
            status = STATUS_SUCCESS;
            break;

        case C_PORT_CONNECTION:
			hprt0.d32 = READ_REGISTER_ULONG((volatile ULONG*)rootHubData->Hprt0);
			hprt0.b.prtconndet = 1;
			WRITE_REGISTER_ULONG((volatile ULONG*)rootHubData->Hprt0, (ULONG)hprt0.d32);

            urb->UrbHeader.Status = USBD_STATUS_SUCCESS;
            status = STATUS_SUCCESS;
            break;

        case C_PORT_RESET:
			rootHubData->ResetState = FALSE;

            urb->UrbHeader.Status = USBD_STATUS_SUCCESS;
            status = STATUS_SUCCESS;
            break;

        case C_PORT_ENABLE:
			hprt0.d32 = READ_REGISTER_ULONG((volatile ULONG*)rootHubData->Hprt0);
			hprt0.b.prtenchng = 1;
			hprt0.b.prtena = 0; // prtena MUST be 0 to not instantly disable the port
			WRITE_REGISTER_ULONG((volatile ULONG*)rootHubData->Hprt0, (ULONG)hprt0.d32);

            urb->UrbHeader.Status = USBD_STATUS_SUCCESS;
            status = STATUS_SUCCESS;
            break;

        case C_PORT_SUSPEND:
            urb->UrbHeader.Status = USBD_STATUS_SUCCESS;
            status = STATUS_SUCCESS;
            break;

        case C_PORT_OVER_CURRENT:
			hprt0.d32 = READ_REGISTER_ULONG((volatile ULONG*)rootHubData->Hprt0);
			hprt0.b.prtovrcurrchng = 1;
			WRITE_REGISTER_ULONG((volatile ULONG*)rootHubData->Hprt0, (ULONG)hprt0.d32);

            urb->UrbHeader.Status = USBD_STATUS_SUCCESS;
            status = STATUS_SUCCESS;
            break;

        default:
            urb->UrbHeader.Status = USBD_STATUS_STALL_PID;
            status = STATUS_UNSUCCESSFUL;
            return;
        }

    } __finally {

        WdfRequestComplete(WdfRequest, status);
    }

    return;
}

VOID
RootHub_UcxEvtSetPortFeature(
    UCXROOTHUB  UcxRootHub,
    WDFREQUEST  WdfRequest
    )
/*++

Routine Description:


Arguments:

    UcxRootHub - The UCX root hub object.

    WdfRequest - The WDF request object for the Set Port Feature request.

Return Value:

    None directly from this routine.

    The request completion status in the WdfRequest and the USBD_STATUS
    in the urb header.

--*/
{
    NTSTATUS                        status = STATUS_SUCCESS;
    PROOTHUB_DATA                   rootHubData;
    WDF_REQUEST_PARAMETERS          wdfRequestParams;
    PURB                            urb;
    PWDF_USB_CONTROL_SETUP_PACKET   setupPacket;
    ULONG                           portNumber;
    ULONG                           featureSelector;
    ULONG                           featureSpecificValue;

	KdPrint((__FUNCTION__ "\n"));

	hprt0_data_t					hprt0;

    __try {

        rootHubData = RootHubGetData(UcxRootHub);

        WDF_REQUEST_PARAMETERS_INIT(&wdfRequestParams);
        WdfRequestGetParameters(WdfRequest, &wdfRequestParams);

        urb = (PURB)wdfRequestParams.Parameters.Others.Arg1;
        setupPacket = (PWDF_USB_CONTROL_SETUP_PACKET)&urb->UrbControlTransferEx.SetupPacket[0];

        //
        // USB Specification 2.0, 11.24.2.13 Set Port Feature
        // USB Specification 3.0, 10.14.2.10 Set Port Feature
        //
        featureSelector      = setupPacket->Packet.wValue.Value;
        portNumber           = setupPacket->Packet.wIndex.Bytes.LowByte;
        featureSpecificValue = setupPacket->Packet.wIndex.Bytes.HiByte;

        if ((setupPacket->Packet.bm.Byte != 0x23) ||
            (setupPacket->Packet.bRequest != USB_REQUEST_SET_FEATURE) ||
            (portNumber == 0) ||
            (portNumber > 1) ||
            (setupPacket->Packet.wLength != 0)) {

            urb->UrbHeader.Status = USBD_STATUS_STALL_PID;
            status = STATUS_UNSUCCESSFUL;
			return;
        }

        //
        // The most significant byte of wIndex is zero, except when
        // the feature selector is PORT_TEST or PORT_INDICATOR.
        //
        if ((featureSelector != PORT_TEST) && (featureSelector != PORT_INDICATOR) &&
                (featureSpecificValue != 0)) {
            urb->UrbHeader.Status = USBD_STATUS_STALL_PID;
            status = STATUS_UNSUCCESSFUL;
			return;
        }

        switch (featureSelector) {

        case PORT_RESET:
			hprt0.d32 = READ_REGISTER_ULONG((volatile ULONG*)rootHubData->Hprt0);
			//hprt0.b.prtconndet = 0;
			//hprt0.b.prtena = 0;
			//hprt0.b.prtenchng = 0;
			//hprt0.b.prtovrcurrchng = 0;
			hprt0.b.prtrst = 1;

			_DataSynchronizationBarrier();
			WRITE_REGISTER_ULONG((volatile ULONG*)rootHubData->Hprt0, (ULONG)hprt0.d32);
			_DataSynchronizationBarrier();

			rootHubData->ResetState = TRUE;

			UcxRootHubPortChanged(UcxRootHub);

			//
			// Software shall ensure that resume is signaled for at
			// least 20 ms (TDRSMDN).  Software shall start timing
			// TDRSMDN from the write of '15' (Resume) to PLS.
			// After TDRSMDN is complete, software shall write a '0'
			// (U0) to the PLS field.
			//
			ExSetTimer(rootHubData->ExTimerResetComplete,
				WDF_REL_TIMEOUT_IN_MS(50),
				0,
				NULL);

            urb->UrbHeader.Status = USBD_STATUS_SUCCESS;
            status = STATUS_SUCCESS;
            break;

        case PORT_SUSPEND:

			hprt0.d32 = READ_REGISTER_ULONG((volatile ULONG*)rootHubData->Hprt0);
			hprt0.b.prtconndet = 0;
			hprt0.b.prtena = 0;
			hprt0.b.prtenchng = 0;
			hprt0.b.prtovrcurrchng = 0;
			hprt0.b.prtsusp = 1;
			WRITE_REGISTER_ULONG((volatile ULONG*)rootHubData->Hprt0, (ULONG)hprt0.d32);

            urb->UrbHeader.Status = USBD_STATUS_SUCCESS;
            status = STATUS_SUCCESS;
            break;

        case PORT_POWER:
			hprt0.d32 = READ_REGISTER_ULONG((volatile ULONG*)rootHubData->Hprt0);
			//hprt0.b.prtconndet = 0;
			//hprt0.b.prtena = 0;
			//hprt0.b.prtenchng = 0;
			hprt0.b.prtovrcurrchng = 0;
			hprt0.b.prtovrcurract = 0;
			hprt0.b.prtpwr = 1;
			WRITE_REGISTER_ULONG((volatile ULONG*)rootHubData->Hprt0, (ULONG)hprt0.d32);

            urb->UrbHeader.Status = USBD_STATUS_SUCCESS;
            status = STATUS_SUCCESS;
            break;

        case PORT_TEST:
            urb->UrbHeader.Status = USBD_STATUS_SUCCESS;
            status = STATUS_SUCCESS;
            break;

        case PORT_INDICATOR:
            urb->UrbHeader.Status = USBD_STATUS_SUCCESS;
            status = STATUS_SUCCESS;
            break;

        default:
			KdPrint(("Unknown port feature %d\n", featureSelector));
            urb->UrbHeader.Status = USBD_STATUS_STALL_PID;
            status = STATUS_UNSUCCESSFUL;
			return;
        }

    } __finally {

        WdfRequestComplete(WdfRequest, status);
    }

    return;
}

VOID
RootHub_UcxEvtGetPortErrorCount(
    UCXROOTHUB  UcxRootHub,
    WDFREQUEST  WdfRequest
    )
/*++

Routine Description:


Arguments:

    UcxRootHub - The UCX root hub object.

    WdfRequest - The WDF request object for the Get Port Error Count request.

Return Value:

    None directly from this routine.

    The request completion status in the WdfRequest and the USBD_STATUS
    in the urb header.

--*/
{
    NTSTATUS                        status = STATUS_SUCCESS;
    PROOTHUB_DATA                   rootHubData;
    WDF_REQUEST_PARAMETERS          wdfRequestParams;
    PURB                            urb;
    PWDF_USB_CONTROL_SETUP_PACKET   setupPacket;
    ULONG                           portNumber;

	KdPrint((__FUNCTION__ "\n"));

    __try {

        rootHubData = RootHubGetData(UcxRootHub);

        WDF_REQUEST_PARAMETERS_INIT(&wdfRequestParams);
        WdfRequestGetParameters(WdfRequest, &wdfRequestParams);

        urb = (PURB)wdfRequestParams.Parameters.Others.Arg1;
        setupPacket = (PWDF_USB_CONTROL_SETUP_PACKET)&urb->UrbControlTransferEx.SetupPacket[0];

        //
        // USB Specification 3.0, 10.14.2.5 Get Port Error Count
        // See also: Q1'09 USB 3.0 Errata (Released 05/15/2009)
        // Correction: bmRequestype must be 10100011B
        //
        portNumber = setupPacket->Packet.wIndex.Value;

        if ((setupPacket->Packet.bm.Byte != 0xA3) ||
            (setupPacket->Packet.bRequest != USB_REQUEST_GET_PORT_ERR_COUNT) ||
            (setupPacket->Packet.wValue.Value != 0) ||
            (portNumber == 0) ||
            (portNumber > 1) ||
            (setupPacket->Packet.wLength != 2)) {

            urb->UrbHeader.Status = USBD_STATUS_STALL_PID;
            status = STATUS_UNSUCCESSFUL;

			return;
        }

        urb->UrbHeader.Status = USBD_STATUS_SUCCESS;
        status = STATUS_SUCCESS;
		*((PUSHORT)urb->UrbControlTransferEx.TransferBuffer) = 0;

    } __finally {

        NT_ASSERT(status == STATUS_SUCCESS);
        WdfRequestComplete(WdfRequest, status);
    }

    return;
}

VOID
RootHub_UcxEvtSetHubFeature(
    UCXROOTHUB  UcxRootHub,
    WDFREQUEST  WdfRequest
    )
/*++

Routine Description:


Arguments:

    UcxRootHub - The UCX root hub object.

    WdfRequest - The WDF request object for the Set Hub Feature request.

Return Value:

    None directly from this routine.

    The request completion status in the WdfRequest and the USBD_STATUS
    in the urb header.

--*/
{
    NTSTATUS                status = STATUS_SUCCESS;
    WDF_REQUEST_PARAMETERS  wdfRequestParams;
    PURB                    urb;

    UNREFERENCED_PARAMETER(UcxRootHub);

	KdPrint((__FUNCTION__ "\n"));

    __try {

        WDF_REQUEST_PARAMETERS_INIT(&wdfRequestParams);
        WdfRequestGetParameters(WdfRequest, &wdfRequestParams);

        urb = (PURB)wdfRequestParams.Parameters.Others.Arg1;

        //
        // USB Specification 2.0, 11.24.2.6 Get Hub Status
        // USB Specification 3.0, 10.14.2.4 Get Hub Status
        //
        // "Hubs may allow setting of these change bits with
        // SetHubFeature() requests in order to support diagnostics.  If
        // the hub does not support setting of these bits, it should
        // either treat the SetHubFeature() request as a Request Error
        // or as a functional no-operation."
        //
        urb->UrbHeader.Status = USBD_STATUS_SUCCESS;
        status = STATUS_SUCCESS;

    } __finally {

        //NT_ASSERT(status == STATUS_SUCCESS);
        WdfRequestComplete(WdfRequest, status);
    }

    return;
}

VOID
RootHub_UcxEvtGetHubStatus(
    UCXROOTHUB  UcxRootHub,
    WDFREQUEST  WdfRequest
    )
/*++

Routine Description:


Arguments:

    UcxRootHub - The UCX root hub object.

    WdfRequest - The WDF request object for the Get Hub Status request.

Return Value:

    None directly from this routine.

    The request completion status in the WdfRequest and the USBD_STATUS
    in the urb header.

--*/
{
    NTSTATUS                        status = STATUS_SUCCESS;
    PROOTHUB_DATA                   rootHubData;
    WDF_REQUEST_PARAMETERS          wdfRequestParams;
    PURB                            urb;
    PWDF_USB_CONTROL_SETUP_PACKET   setupPacket;

    UNREFERENCED_PARAMETER(UcxRootHub);

	KdPrint((__FUNCTION__ "\n"));

    __try {

        rootHubData = RootHubGetData(UcxRootHub);

        WDF_REQUEST_PARAMETERS_INIT(&wdfRequestParams);
        WdfRequestGetParameters(WdfRequest, &wdfRequestParams);

        urb = (PURB)wdfRequestParams.Parameters.Others.Arg1;
        setupPacket = (PWDF_USB_CONTROL_SETUP_PACKET)&urb->UrbControlTransferEx.SetupPacket[0];

        //
        // USB Specification 2.0, 11.24.2.6 Get Hub Status
        // USB Specification 3.0, 10.14.2.4 Get Hub Status
        //
        if ((setupPacket->Packet.bm.Byte != 0xA0) ||
            (setupPacket->Packet.bRequest != USB_REQUEST_GET_STATUS) ||
            (setupPacket->Packet.wValue.Value != 0) ||
            (setupPacket->Packet.wIndex.Value != 0) ||
            (setupPacket->Packet.wLength != 4)) {

            urb->UrbHeader.Status = USBD_STATUS_STALL_PID;
            status = STATUS_UNSUCCESSFUL;
            return;
        }

        //
        // The xHCI root hub does not report loss of local power or
        // over-current, or change of local power or change of
        // over-current.
        //
        *((PULONG)urb->UrbControlTransferEx.TransferBuffer) = 0;

        urb->UrbHeader.Status = USBD_STATUS_SUCCESS;
        status = STATUS_SUCCESS;

    } __finally {

        NT_ASSERT(status == STATUS_SUCCESS);
        WdfRequestComplete(WdfRequest, status);
    }

    return;
}

VOID
RootHub_UcxEvtGetPortStatus(
    UCXROOTHUB  UcxRootHub,
    WDFREQUEST  WdfRequest
    )
/*++

Routine Description:


Arguments:

    UcxRootHub - The UCX root hub object.

    WdfRequest - The WDF request object for the Get Port Status request.

Return Value:

    None directly from this routine.

    The request completion status in the WdfRequest and the USBD_STATUS
    in the urb header.

--*/
{
    NTSTATUS                        status = STATUS_SUCCESS;
    PROOTHUB_DATA                   rootHubData;
    WDF_REQUEST_PARAMETERS          wdfRequestParams;
    PURB                            urb;
    PWDF_USB_CONTROL_SETUP_PACKET   setupPacket;
    ULONG                           portNumber;
    ULONG                           packetLength;
    PUSB_PORT_STATUS_AND_CHANGE     pusbPortStatusChange;

	KdPrint((__FUNCTION__ "\n"));

	hprt0_data_t hprt0;

    __try {

        rootHubData = RootHubGetData(UcxRootHub);

        WDF_REQUEST_PARAMETERS_INIT(&wdfRequestParams);
        WdfRequestGetParameters(WdfRequest, &wdfRequestParams);

        urb = (PURB)wdfRequestParams.Parameters.Others.Arg1;
        setupPacket = (PWDF_USB_CONTROL_SETUP_PACKET)&urb->UrbControlTransferEx.SetupPacket[0];

        //
        // USB Specification 2.0, 11.24.2.7 Get Port Status
        // USB Specification 3.0, 10.14.2.6 Get Port Status
        //
        portNumber = setupPacket->Packet.wIndex.Value;
        packetLength = (setupPacket->Packet.wValue.Value == USB_STATUS_EXT_PORT_STATUS) ? 8 : 4;

        if ((setupPacket->Packet.bm.Byte != 0xA3) ||
            (setupPacket->Packet.bRequest != USB_REQUEST_GET_STATUS) ||
            ((setupPacket->Packet.wValue.Value != USB_STATUS_PORT_STATUS) &&
             (setupPacket->Packet.wValue.Value != USB_STATUS_EXT_PORT_STATUS)) ||
            (portNumber == 0) ||
            (portNumber > 1) ||
            (setupPacket->Packet.wLength != packetLength)) {

            urb->UrbHeader.Status = USBD_STATUS_STALL_PID;
            status = STATUS_UNSUCCESSFUL;
            return;
        }

        if ((setupPacket->Packet.wValue.Value == USB_STATUS_PORT_STATUS)) {
            pusbPortStatusChange = (PUSB_PORT_STATUS_AND_CHANGE)urb->UrbControlTransferEx.TransferBuffer;
            pusbPortStatusChange->AsUlong32 = 0;

			hprt0.d32 = READ_REGISTER_ULONG((volatile ULONG*)rootHubData->Hprt0);

            //
            // USB Specification 2.0, 11.24.2.7.1 Port Status Bits
            //
            pusbPortStatusChange->PortStatus.Usb20PortStatus.CurrentConnectStatus =
                (USHORT)hprt0.b.prtconnsts;

            pusbPortStatusChange->PortStatus.Usb20PortStatus.PortEnabledDisabled =
				(USHORT)hprt0.b.prtena;

			pusbPortStatusChange->PortStatus.Usb20PortStatus.Suspend =
				(USHORT)hprt0.b.prtsusp;

			pusbPortStatusChange->PortStatus.Usb20PortStatus.OverCurrent =
				(USHORT)hprt0.b.prtovrcurract;

			pusbPortStatusChange->PortStatus.Usb20PortStatus.Reset =
				(USHORT)hprt0.b.prtrst;

			pusbPortStatusChange->PortStatus.Usb20PortStatus.PortPower =
				(USHORT)hprt0.b.prtpwr;

			pusbPortStatusChange->PortStatus.Usb20PortStatus.HighSpeedDeviceAttached = 
				(USHORT)(hprt0.b.prtspd == 0);

			pusbPortStatusChange->PortChange.Usb20PortChange.ConnectStatusChange =
				(USHORT)hprt0.b.prtconndet;

			pusbPortStatusChange->PortChange.Usb20PortChange.PortEnableDisableChange =
				(USHORT)hprt0.b.prtenchng;

			pusbPortStatusChange->PortChange.Usb20PortChange.OverCurrentIndicatorChange =
				(USHORT)hprt0.b.prtovrcurrchng;

			pusbPortStatusChange->PortChange.Usb20PortChange.ResetChange =
				(USHORT)rootHubData->ResetState;

            urb->UrbHeader.Status = USBD_STATUS_SUCCESS;

            status = STATUS_SUCCESS;

        } else {

            urb->UrbHeader.Status = USBD_STATUS_STALL_PID;
            status = STATUS_UNSUCCESSFUL;
            return;
        }


    } __finally {

        WdfRequestComplete(WdfRequest, status);
    }

    return;
}


VOID
RootHub_ResetComplete(
	__in
	PEX_TIMER  Timer,
	__in_opt
	PVOID      Context
)
{
	UNREFERENCED_PARAMETER(Timer);

	KdPrint((__FUNCTION__ "\n"));

	KeMemoryBarrier();

	PROOTHUB_DATA rootHub = (PROOTHUB_DATA)Context;
	hprt0_data_t hprt0;
	hprt0.d32 = *rootHub->Hprt0;

	KeMemoryBarrier();
	_DataSynchronizationBarrier();

	hprt0.b.prtrst = 0;

	KeMemoryBarrier();
	_DataSynchronizationBarrier();

	WRITE_REGISTER_ULONG((volatile ULONG*)rootHub->Hprt0, (ULONG)hprt0.d32);

	//UcxRootHubPortChanged(rootHub->UcxRootHub);

	ExSetTimer(rootHub->ExTimerResetSafeComplete,
		WDF_REL_TIMEOUT_IN_MS(10),
		0,
		NULL);

	KeMemoryBarrier();
}

VOID
RootHub_ResetSafeComplete(
	__in
	PEX_TIMER  Timer,
	__in_opt
	PVOID      Context
)
{
	UNREFERENCED_PARAMETER(Timer);

	KdPrint((__FUNCTION__ "\n"));

	PROOTHUB_DATA rootHub = (PROOTHUB_DATA)Context;

	KeMemoryBarrier();

	hprt0_data_t hprt0;
	hprt0.d32 = *rootHub->Hprt0;

	KeMemoryBarrier();

	if (hprt0.b.prtenchng || hprt0.b.prtovrcurrchng || hprt0.b.prtconndet/* || rootHubData->ResetState*/)
	{
		UcxRootHubPortChanged(rootHub->UcxRootHub);
	}
	else
	{
		ExSetTimer(rootHub->ExTimerResetSafeComplete,
			WDF_REL_TIMEOUT_IN_MS(10),
			0,
			NULL);
	}
}

VOID
RootHub_ResumeComplete(
	__in
	PEX_TIMER  Timer,
	__in_opt
	PVOID      Context
)
{
	UNREFERENCED_PARAMETER(Timer);

	KdPrint((__FUNCTION__ "\n"));

	PROOTHUB_DATA rootHub = (PROOTHUB_DATA)Context;

	KeMemoryBarrier();

	hprt0_data_t hprt0;
	hprt0.d32 = *rootHub->Hprt0;

	KeMemoryBarrier();
	_DataSynchronizationBarrier();

	hprt0.b.prtsusp = 0;
	hprt0.b.prtres = 0;

	KeMemoryBarrier();
	_DataSynchronizationBarrier();

	*rootHub->Hprt0 = hprt0.d32;

	KeMemoryBarrier();
}

NTSTATUS
RootHubCreate(
	_In_ WDFDEVICE WdfDevice,
	_In_ UCXCONTROLLER UcxController
)
{
	NTSTATUS                status;
	UCX_ROOTHUB_CONFIG      ucxRootHubConfig;
	WDF_OBJECT_ATTRIBUTES   wdfAttributes;
	UCXROOTHUB              ucxRootHub;
	PROOTHUB_DATA           rootHubData;
	PDEVICE_CONTEXT			ctx;

	ctx = DeviceGetContext(WdfDevice);

	KdPrint((__FUNCTION__ "\n"));

	PAGED_CODE();

	UCX_ROOTHUB_CONFIG_INIT(&ucxRootHubConfig,
		RootHub_UcxEvtClearHubFeature,
		RootHub_UcxEvtClearPortFeature,
		RootHub_UcxEvtGetHubStatus,
		RootHub_UcxEvtGetPortStatus,
		RootHub_UcxEvtSetHubFeature,
		RootHub_UcxEvtSetPortFeature,
		RootHub_UcxEvtGetPortErrorCount,
		RootHub_UcxEvtInterruptTransfer,
		RootHub_UcxEvtGetInfo,
		RootHub_UcxEvtGet20PortInfo,
		RootHub_UcxEvtGet30PortInfo);

	WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&ucxRootHubConfig.WdfRequestAttributes, REQUEST_DATA);

	WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&wdfAttributes, ROOTHUB_DATA);

	status = UcxRootHubCreate(UcxController,
		&ucxRootHubConfig,
		&wdfAttributes,
		&ucxRootHub);

	if (NT_SUCCESS(status))
	{
		ControllerGetData(UcxController)->RootHub = ucxRootHub;

		rootHubData = RootHubGetData(ucxRootHub);

		rootHubData->ResetState = FALSE;
		rootHubData->UcxRootHub = ucxRootHub;
		rootHubData->ExTimerResetComplete = ExAllocateTimer(RootHub_ResetComplete, rootHubData, EX_TIMER_HIGH_RESOLUTION);
		rootHubData->ExTimerResumeComplete = ExAllocateTimer(RootHub_ResumeComplete, rootHubData, EX_TIMER_HIGH_RESOLUTION);
		rootHubData->ExTimerResetSafeComplete = ExAllocateTimer(RootHub_ResetSafeComplete, rootHubData, EX_TIMER_HIGH_RESOLUTION);
	}

	return status;
}

NTSTATUS
RootHubInit(
	UCXCONTROLLER UcxController
)
{
	PCONTROLLER_DATA controllerData = ControllerGetData(UcxController);
	PDEVICE_CONTEXT ctx = DeviceGetContext(controllerData->WdfDevice);
	PROOTHUB_DATA rootHubData = RootHubGetData(controllerData->RootHub);
	LARGE_INTEGER coreBase;
	coreBase.QuadPart = ctx->MemoryBase.QuadPart + 0x0;

	LARGE_INTEGER hostBase;
	hostBase.QuadPart = ctx->MemoryBase.QuadPart + 0x400;

	LARGE_INTEGER hprt0Base;
	hprt0Base.QuadPart = ctx->MemoryBase.QuadPart + 0x440;

	rootHubData->CoreGlobalRegs = MmMapIoSpace(coreBase, sizeof(dwc_otg_core_global_regs_t), MmNonCached);
	rootHubData->HostGlobalRegs = MmMapIoSpace(hostBase, sizeof(dwc_otg_host_global_regs_t), MmNonCached);
	rootHubData->Hprt0 = MmMapIoSpace(hprt0Base, sizeof(uint32_t), MmNonCached);

	hprt0_data_t hprt0;
	hprt0.d32 = READ_REGISTER_ULONG((volatile ULONG*)rootHubData->Hprt0);

	KeMemoryBarrier();
	_DataSynchronizationBarrier();

	if (hprt0.b.prtpwr == 0)
	{
		hprt0.b.prtpwr = 1;

		KeMemoryBarrier();
		_DataSynchronizationBarrier();

		*rootHubData->Hprt0 = hprt0.d32;
	}

	return STATUS_SUCCESS;
}

NTSTATUS
UsbDevice_UcxEvtDeviceAdd(
	UCXCONTROLLER       UcxController,
	PUCXUSBDEVICE_INFO  UsbDeviceInfo,
	PUCXUSBDEVICE_INIT  UsbDeviceInit
);

NTSTATUS
Controller_UcxEvtGetCurrentFrameNumber(
	UCXCONTROLLER   UcxController,
	PULONG          FrameNumber
)

{
	PCONTROLLER_DATA    controllerData;
	controllerData = ControllerGetData(UcxController);

	KdPrint((__FUNCTION__ "\n"));

	KeMemoryBarrier();
	_DataSynchronizationBarrier();

	hfnum_data_t hfnum;
	hfnum.d32 = controllerData->HostGlobalRegs->hfnum;

	KeMemoryBarrier();
	_DataSynchronizationBarrier();

	*FrameNumber = hfnum.b.frnum;

	//*FrameNumber = 0xFFFFFFFF;

	return STATUS_SUCCESS;
}

VOID
Controller_Reset(
	UCXCONTROLLER   UcxController
)
{
	PCONTROLLER_DATA    controllerData;
	grstctl_t grst;

	KdPrint((__FUNCTION__ "\n"));

	controllerData = ControllerGetData(UcxController);

	// wait for AHBIDLE
	LARGE_INTEGER delay;
	delay.QuadPart = 100; // times 100 nanoseconds == 10 us

	for (unsigned int i = 0; i < 100000; i++)
	{
		KeDelayExecutionThread(KernelMode, FALSE, &delay);
		KeMemoryBarrier();

		grst.d32 = controllerData->CoreGlobalRegs->grstctl;

		if (grst.b.ahbidle)
		{
			break;
		}
	}

	// set CSFTRST
	delay.QuadPart = 10; // times 100 nanoseconds == 1 us

	grst.b.csftrst = 1;
	controllerData->CoreGlobalRegs->grstctl = grst.d32;

	KeMemoryBarrier();

	for (unsigned int i = 0; i < 10000; i++)
	{
		KeDelayExecutionThread(KernelMode, FALSE, &delay);
		KeMemoryBarrier();

		grst.d32 = controllerData->CoreGlobalRegs->grstctl;

		if (!grst.b.csftrst)
		{
			break;
		}
	}
}

VOID
Controller_UcxEvtReset(
	UCXCONTROLLER   UcxController
)
{
	UCX_CONTROLLER_RESET_COMPLETE_INFO  ucxControllerResetCompleteInfo;

	Controller_Reset(UcxController);

	UCX_CONTROLLER_RESET_COMPLETE_INFO_INIT(&ucxControllerResetCompleteInfo,
		UcxControllerStateLost,
		TRUE);

	UcxControllerResetComplete(UcxController, &ucxControllerResetCompleteInfo);
}

NTSTATUS
Controller_UcxEvtQueryUsbCapability(
	UCXCONTROLLER   UcxController,
	PGUID           CapabilityType,
	ULONG           OutputBufferLength,
	PVOID           OutputBuffer,
	PULONG          ResultLength
)

{
	NTSTATUS status;

	UNREFERENCED_PARAMETER(UcxController);
	UNREFERENCED_PARAMETER(OutputBufferLength);
	UNREFERENCED_PARAMETER(OutputBuffer);

	KdPrint((__FUNCTION__ "\n"));

	*ResultLength = 0;

	if (RtlCompareMemory(CapabilityType,
		&GUID_USB_CAPABILITY_CHAINED_MDLS,
		sizeof(GUID)) == sizeof(GUID)) {

		status = STATUS_NOT_SUPPORTED;
	}
	else if (RtlCompareMemory(CapabilityType,
		&GUID_USB_CAPABILITY_STATIC_STREAMS,
		sizeof(GUID)) == sizeof(GUID)) {

		status = STATUS_NOT_SUPPORTED;
	}
	else if (RtlCompareMemory(CapabilityType,
		&GUID_USB_CAPABILITY_FUNCTION_SUSPEND,
		sizeof(GUID)) == sizeof(GUID)) {

		status = STATUS_NOT_SUPPORTED;
	}
	else if (RtlCompareMemory(CapabilityType,
		&GUID_USB_CAPABILITY_SELECTIVE_SUSPEND,
		sizeof(GUID)) == sizeof(GUID)) {
		status = STATUS_NOT_SUPPORTED;
	}
	else if (RtlCompareMemory(CapabilityType,
		&GUID_USB_CAPABILITY_DEVICE_CONNECTION_HIGH_SPEED_COMPATIBLE,
		sizeof(GUID)) == sizeof(GUID)) {
		status = STATUS_SUCCESS;
	}
	else if (RtlCompareMemory(CapabilityType,
		&GUID_USB_CAPABILITY_DEVICE_CONNECTION_SUPER_SPEED_COMPATIBLE,
		sizeof(GUID)) == sizeof(GUID)) {
		status = STATUS_NOT_SUPPORTED;
	}
	else if (RtlCompareMemory(CapabilityType,
		&GUID_USB_CAPABILITY_CLEAR_TT_BUFFER_ON_ASYNC_TRANSFER_CANCEL,
		sizeof(GUID)) == sizeof(GUID)) {

		status = STATUS_NOT_SUPPORTED;
	}
	else {
		status = STATUS_NOT_IMPLEMENTED;
	}

	return status;
}

_Use_decl_annotations_
BOOLEAN OnInterruptIsr(WDFINTERRUPT WdfInterrupt, ULONG MessageID)
{
	UNREFERENCED_PARAMETER(MessageID);

	PINTERRUPT_CONTEXT context = InterruptGetData(WdfInterrupt);

	gintsts_data_t gintsts;

	KeMemoryBarrier();
	_DataSynchronizationBarrier();

	gintsts.d32 = context->ControllerHandle->CoreGlobalRegs->gintsts;

	if (gintsts.b.hcintr)
	{
		// to not re-trigger the interrupt constantly
		context->ControllerHandle->HostGlobalRegs->haintmsk = (~context->ControllerHandle->HostGlobalRegs->haint) & 0xFFFF;

		KeMemoryBarrier();
		_DataSynchronizationBarrier();

		WdfInterruptQueueWorkItemForIsr(WdfInterrupt);
		return TRUE;
	}
	else if (gintsts.b.portintr || gintsts.b.disconnect)
	{
		WdfInterruptQueueWorkItemForIsr(WdfInterrupt);
		return TRUE;
	}
	else if (gintsts.b.rxstsqlvl)
	{
		// this should never fire in DMA mode, which is the only mode we support
		KdPrint(("WTF??? Got rxstsqlvl in DMA mode?!\n"));
		return TRUE;
	}
	else if (gintsts.b.sessreqintr)
	{
		WdfInterruptQueueWorkItemForIsr(WdfInterrupt);
		return TRUE;
	}

	return FALSE;
}

_Use_decl_annotations_
VOID OnInterruptWorkItem(WDFINTERRUPT WdfInterrupt, WDFOBJECT WdfDevice)
{
	UNREFERENCED_PARAMETER(WdfDevice);

	PINTERRUPT_CONTEXT context = InterruptGetData(WdfInterrupt);

	gintsts_data_t gintsts;

	KeMemoryBarrier();
	_DataSynchronizationBarrier();

	gintsts.d32 = context->ControllerHandle->CoreGlobalRegs->gintsts;

	//KdPrint((__FUNCTION__ "\n"));

	if (gintsts.b.hcintr)
	{
		//KdPrint(("hcintr\n"));

		uint32_t haint = context->ControllerHandle->HostGlobalRegs->haint;

		for (int i = 0; i < 16; i++)
		{
			if (haint & (1 << i))
			{
				PFN_CHANNEL_CALLBACK cb = context->ControllerHandle->ChannelCallbacks[i];

				if (cb)
				{
					cb(context->ControllerHandle->ChannelCallbackContext[i]);
				}
			}
		}
	}
	else if (gintsts.b.portintr || gintsts.b.disconnect)
	{
		UcxRootHubPortChanged(context->ControllerHandle->RootHub);
	}
	else if (gintsts.b.sessreqintr)
	{
		// turn on port power
		hprt0_data_t hprt0;
		ROOTHUB_DATA *hubctx = RootHubGetData(context->ControllerHandle->RootHub);
		hprt0.d32 = READ_REGISTER_ULONG((volatile ULONG *)hubctx->Hprt0);
		hprt0.b.prtena = 0;
		hprt0.b.prtconndet = 0;
		hprt0.b.prtenchng = 0;
		hprt0.b.prtovrcurrchng = 0;
		hprt0.b.prtpwr = 1;
		WRITE_REGISTER_ULONG((volatile ULONG *)hubctx->Hprt0, hprt0.d32);

		// and then clear the interrupt so it doesn't fire again (at least that's what the Linux driver says this is for...)
		gintsts.d32 = 0;
		gintsts.b.sessreqintr = 1;
		WRITE_REGISTER_ULONG((volatile ULONG *)context->ControllerHandle->CoreGlobalRegs->gintsts, gintsts.d32);
	}
}

void DeviceSystemThread(
	PVOID StartContext
)
{
	UNREFERENCED_PARAMETER(StartContext);
	/*PCONTROLLER_DATA data = StartContext;

	while (TRUE)
	{
		LARGE_INTEGER interval;
		interval.QuadPart = 0;

		KeDelayExecutionThread(KernelMode, FALSE, &interval);

		gintsts_data_t gintsts;

		KeMemoryBarrier();
		_DataSynchronizationBarrier();

		gintsts.d32 = data->CoreGlobalRegs->gintsts;

		if (gintsts.b.hcintr)
		{
			KdPrint(("hcintr\n"));

			uint32_t haint = data->HostGlobalRegs->haint;

			for (int i = 0; i < 16; i++)
			{
				if (haint & (1 << i))
				{
					PFN_CHANNEL_CALLBACK cb = data->ChannelCallbacks[i];

					if (cb)
					{
						cb(data->ChannelCallbackContext[i]);
					}
				}
			}
		}
	}*/
}

VOID
Controller_ResumeCh(
	_In_ PEX_TIMER Timer,
	_In_ PVOID Context
);

NTSTATUS
ControllerCreate(
	_In_ WDFDEVICE WdfDevice,
	_Out_ UCXCONTROLLER* Controller
)
{
	PAGED_CODE();

	KdPrint((__FUNCTION__ "\n"));

	UCX_CONTROLLER_CONFIG                   ucxControllerConfig;
	WDF_OBJECT_ATTRIBUTES                   wdfAttributes;
	UCXCONTROLLER                           ucxController;
	NTSTATUS status = STATUS_SUCCESS;

	WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&wdfAttributes, CONTROLLER_DATA);

	UCX_CONTROLLER_CONFIG_INIT(&ucxControllerConfig, "DWUSB");

	ucxControllerConfig.EvtControllerUsbDeviceAdd = UsbDevice_UcxEvtDeviceAdd;
	ucxControllerConfig.EvtControllerGetCurrentFrameNumber = Controller_UcxEvtGetCurrentFrameNumber;
	ucxControllerConfig.EvtControllerReset = Controller_UcxEvtReset;
	ucxControllerConfig.EvtControllerQueryUsbCapability = Controller_UcxEvtQueryUsbCapability;
	ucxControllerConfig.ParentBusType = UcxControllerParentBusTypeAcpi;
	UCX_CONTROLLER_CONFIG_SET_ACPI_INFO(&ucxControllerConfig, "BCM", "2848", "0");

	status = UcxControllerCreate(WdfDevice,
		&ucxControllerConfig,
		&wdfAttributes,
		&ucxController);

	if (!NT_SUCCESS(status)) {
		return status;
	}

	PCONTROLLER_DATA controllerData = ControllerGetData(ucxController);
	controllerData->WdfDevice = WdfDevice;
	controllerData->UsbAddressInit = 0;
	controllerData->ChannelMask = 0;
	status = WdfSpinLockCreate(WDF_NO_OBJECT_ATTRIBUTES, &controllerData->ChannelMaskLock);
	if (!NT_SUCCESS(status)) {
		return status;
	}

	PDEVICE_CONTEXT ctx = DeviceGetContext(WdfDevice);

	for (int i = 0; i < 8; i++)
	{
		controllerData->ChResumeTimers[i] = ExAllocateTimer(Controller_ResumeCh, &controllerData->ChResumeContexts[i], EX_TIMER_HIGH_RESOLUTION);
	}

	LARGE_INTEGER coreBase;
	coreBase.QuadPart = ctx->MemoryBase.QuadPart + 0x0;

	LARGE_INTEGER hostBase;
	hostBase.QuadPart = ctx->MemoryBase.QuadPart + 0x400;

	LARGE_INTEGER pcgcBase;
	pcgcBase.QuadPart = ctx->MemoryBase.QuadPart + 0xE00;

	controllerData->CoreGlobalRegs = MmMapIoSpace(coreBase, sizeof(dwc_otg_core_global_regs_t), MmNonCached);
	controllerData->HostGlobalRegs = MmMapIoSpace(hostBase, sizeof(dwc_otg_host_global_regs_t), MmNonCached);
	controllerData->PcgcCtl = MmMapIoSpace(pcgcBase, sizeof(uint32_t), MmNonCached);

	for (int i = 0; i < 16; i++)
	{
		controllerData->ChannelCallbacks[i] = NULL;
		controllerData->ChannelCallbackContext[i] = NULL;
	}

	*Controller = ucxController;

	controllerData->WdfInterrupt = ctx->Interrupt;

	WDF_DMA_ENABLER_CONFIG   dmaConfig;

	WDF_DMA_ENABLER_CONFIG_INIT(&dmaConfig,
		WdfDmaProfilePacket,
		65536);

	/*WdfDeviceSetAlignmentRequirement(WdfDevice, 1023);

	WDFDMAENABLER dmaEnabler;

	status = WdfDmaEnablerCreate(WdfDevice,
		&dmaConfig,
		WDF_NO_OBJECT_ATTRIBUTES,
		&dmaEnabler);

	if (!NT_SUCCESS(status))
	{
		return status;
	}

	WDFCOMMONBUFFER writeCommonBuffer;

	status = WdfCommonBufferCreate(dmaEnabler,
		65536,
		WDF_NO_OBJECT_ATTRIBUTES,
		&writeCommonBuffer);

	controllerData->CommonBufferBase =
		WdfCommonBufferGetAlignedVirtualAddress(
			writeCommonBuffer);
	controllerData->CommonBufferBaseLA =
		WdfCommonBufferGetAlignedLogicalAddress(
			writeCommonBuffer);

	RtlZeroMemory(controllerData->CommonBufferBase, 65536);*/

#define HEX_1_G                     0x40000000
#define OFFSET_DIRECT_SDRAM			0xC0000000

	PHYSICAL_ADDRESS HighestAcceptableAddress = { 0 };
	PHYSICAL_ADDRESS LowestAcceptableAddress = { 0 };
	PHYSICAL_ADDRESS BoundaryAddress = { 0 };
	PHYSICAL_ADDRESS addrProperty;

	for (int i = 0; i < 8; i++)
	{
		HighestAcceptableAddress.QuadPart = HEX_1_G;

		controllerData->CommonBufferBase[i] = MmAllocateContiguousNodeMemory(
			65536,
			LowestAcceptableAddress,
			HighestAcceptableAddress,
			BoundaryAddress,
			PAGE_NOCACHE | PAGE_READWRITE,
			MM_ANY_NODE_OK
		);

		addrProperty = MmGetPhysicalAddress(controllerData->CommonBufferBase[i]);
		addrProperty.LowPart += OFFSET_DIRECT_SDRAM;

		controllerData->CommonBufferBaseLA[i] = addrProperty;

		RtlZeroMemory(controllerData->CommonBufferBase[i], 65536);
	}

	InterruptGetData(controllerData->WdfInterrupt)->ControllerHandle = controllerData;

	return STATUS_SUCCESS;
}

/**
* This function calculates the number of IN EPS
* using GHWCFG1 and GHWCFG2 registers values
*
* @param core_if Programming view of the DWC_otg controller
*/
static uint32_t calc_num_in_eps(PCONTROLLER_DATA core_if)
{
	uint32_t num_in_eps = 0;
	uint32_t num_eps = core_if->hwcfg2.b.num_dev_ep;
	uint32_t hwcfg1 = core_if->hwcfg1.d32 >> 3;
	uint32_t num_tx_fifos = core_if->hwcfg4.b.num_in_eps;
	unsigned int i;

	for (i = 0; i < num_eps; ++i) {
		if (!(hwcfg1 & 0x1))
			num_in_eps++;

		hwcfg1 >>= 2;
	}

	if (core_if->hwcfg4.b.ded_fifo_en) {
		num_in_eps =
			(num_in_eps > num_tx_fifos) ? num_tx_fifos : num_in_eps;
	}

	return num_in_eps;
}

/**
* This function calculates the number of OUT EPS
* using GHWCFG1 and GHWCFG2 registers values
*
* @param core_if Programming view of the DWC_otg controller
*/
static uint32_t calc_num_out_eps(PCONTROLLER_DATA core_if)
{
	uint32_t num_out_eps = 0;
	uint32_t num_eps = core_if->hwcfg2.b.num_dev_ep;
	uint32_t hwcfg1 = core_if->hwcfg1.d32 >> 2;
	unsigned int i;

	for (i = 0; i < num_eps; ++i) {
		if (!(hwcfg1 & 0x1))
			num_out_eps++;

		hwcfg1 >>= 2;
	}
	return num_out_eps;
}

NTSTATUS
ControllerInit(
	UCXCONTROLLER ucxController
)
{
	PCONTROLLER_DATA controllerData = ControllerGetData(ucxController);
	NTSTATUS status = STATUS_SUCCESS;

	controllerData->hwcfg1.d32 = controllerData->CoreGlobalRegs->ghwcfg1;
	controllerData->hwcfg2.d32 = controllerData->CoreGlobalRegs->ghwcfg2;
	controllerData->hwcfg3.d32 = controllerData->CoreGlobalRegs->ghwcfg3;
	controllerData->hwcfg4.d32 = controllerData->CoreGlobalRegs->ghwcfg4;
	controllerData->hptxfsiz.d32 = controllerData->CoreGlobalRegs->hptxfsiz;

	gusbcfg_data_t gusbcfg;
	gusbcfg.d32 = controllerData->CoreGlobalRegs->gusbcfg;

	gusbcfg.b.ulpi_ext_vbus_drv = 0;// 1;
	gusbcfg.b.term_sel_dl_pulse = 0;

	controllerData->CoreGlobalRegs->gusbcfg = gusbcfg.d32;

	_DataSynchronizationBarrier();
	KeMemoryBarrier();

	Controller_Reset(ucxController);

	// set PHY config
	gusbcfg.d32 = controllerData->CoreGlobalRegs->gusbcfg;

	gusbcfg.b.ddrsel = 0;
	gusbcfg.b.ulpi_utmi_sel = 1;
	gusbcfg.b.phyif = 0;

	controllerData->CoreGlobalRegs->gusbcfg = gusbcfg.d32;

	_DataSynchronizationBarrier();
	KeMemoryBarrier();

	// reset, again
	Controller_Reset(ucxController);

	gusbcfg.d32 = controllerData->CoreGlobalRegs->gusbcfg;

	gusbcfg.b.ulpi_fsls = 0;
	gusbcfg.b.ulpi_clk_sus_m = 0;
	gusbcfg.b.hnpcap = 1;
	gusbcfg.b.srpcap = 1;

	controllerData->CoreGlobalRegs->gusbcfg = gusbcfg.d32;

	*controllerData->PcgcCtl = 0;
	KeMemoryBarrier();

	gahbcfg_data_t gahbcfg;
	gahbcfg.d32 = 0;

	switch (controllerData->hwcfg2.b.architecture) {

	case DWC_SLAVE_ONLY_ARCH:
		return STATUS_NOT_SUPPORTED;
	case DWC_EXT_DMA_ARCH:
		//DWC_DEBUGPL(DBG_CIL, "External DMA Mode\n");
		{
			unsigned char brst_sz = 32;
			gahbcfg.b.hburstlen = 0;
			while (brst_sz > 1) {
				gahbcfg.b.hburstlen++;
				brst_sz >>= 1;
			}
		}
		break;

	case DWC_INT_DMA_ARCH:
		//DWC_DEBUGPL(DBG_CIL, "Internal DMA Mode\n");
		/* Old value was DWC_GAHBCFG_INT_DMA_BURST_INCR - done for
		Host mode ISOC in issue fix - vahrama */
		/* Broadcom had altered to (1<<3)|(0<<0) - WRESP=1, max 4 beats */
		gahbcfg.b.hburstlen = (1 << 3) | (0 << 0);//DWC_GAHBCFG_INT_DMA_BURST_INCR4;
		break;

	}

	gahbcfg.b.glblintrmsk = 1;
	gahbcfg.b.dmaenable = 1;

	controllerData->CoreGlobalRegs->gahbcfg = gahbcfg.d32;

	hcfg_data_t hcfg;
	hcfg.d32 = controllerData->HostGlobalRegs->hcfg;
	hcfg.b.fslspclksel = DWC_HCFG_30_60_MHZ;
	controllerData->HostGlobalRegs->hcfg = hcfg.d32;

	gotgctl_data_t gotgctl;
	gotgctl.d32 = controllerData->CoreGlobalRegs->gotgctl;

	gotgctl.b.hstsethnpen = 1;// 0;

	controllerData->CoreGlobalRegs->gotgctl = gotgctl.d32;

	controllerData->CoreGlobalRegs->gotgint = 0xFFFFFFFF;
	controllerData->CoreGlobalRegs->gintsts = 0xFFFFFFFF;


	gintmsk_data_t gintmsk;
	gintmsk.d32 = 0;
	//gintmsk.b.modemismatch = 1; // OTG modeswitching related; we only support host mode
	//gintmsk.b.otgintr = 1; // also OTG modeswitching
	//gintmsk.b.rxstsqlvl = 1; // should be enabled only for PIO slave mode (we only support DMA mode for now)
	//gintmsk.b.conidstschng = 1; // also OTG modeswitching
	//gintmsk.b.wkupintr = 1; // for now, as we don't support any low-power modes or wake-on-USB
	//gintmsk.b.usbsuspend = 1; // ditto
	gintmsk.b.sessreqintr = 1;
	gintmsk.b.disconnect = 1; // "Disconnect from root port" - do we need this? can this ever happen on the Pi?
	gintmsk.b.hcintr = 1;
	gintmsk.b.portintr = 1;
	controllerData->CoreGlobalRegs->gintmsk = gintmsk.d32;

	controllerData->HostGlobalRegs->haintmsk = 0x1;

	HANDLE threadHandle;

	status = PsCreateSystemThread(&threadHandle, SYNCHRONIZE, NULL, NULL, NULL, DeviceSystemThread, controllerData);

	if (!NT_SUCCESS(status))
	{
		return status;
	}

	ZwClose(threadHandle);

	return status;
}

NTSTATUS
dwusbProbeResources(
	PDEVICE_CONTEXT ctx,
	WDFCMRESLIST res,
	WDFCMRESLIST rawres
)
{
	NTSTATUS status = STATUS_SUCCESS;
	PCM_PARTIAL_RESOURCE_DESCRIPTOR  desc;
	WDF_INTERRUPT_CONFIG Config;
	WDF_OBJECT_ATTRIBUTES interruptObjectAttributes;
	BOOLEAN HaveInterrupt = FALSE;
	BOOLEAN HaveMemory = FALSE;
	for (unsigned int i = 0; i < WdfCmResourceListGetCount(res); i++) {
		desc = WdfCmResourceListGetDescriptor(res, i);
		switch (desc->Type) {

		case CmResourceTypeMemory:
		case CmResourceTypeMemoryLarge:
			//
			// Handle memory resources here.
			//
			ctx->MemoryBase = desc->u.Memory.Start;
			HaveMemory = TRUE;
			break;

		case CmResourceTypeInterrupt:
			WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(
				&interruptObjectAttributes,
				INTERRUPT_CONTEXT);
			WDF_INTERRUPT_CONFIG_INIT(&Config, OnInterruptIsr, NULL);
			Config.PassiveHandling = TRUE;
			Config.AutomaticSerialization = TRUE;
			Config.EvtInterruptWorkItem = OnInterruptWorkItem;
			Config.InterruptRaw = WdfCmResourceListGetDescriptor(rawres, i);
			Config.InterruptTranslated = desc;
			status = WdfInterruptCreate(ctx->Device, &Config, &interruptObjectAttributes, &ctx->Interrupt);
			HaveInterrupt = TRUE;
			if (!NT_SUCCESS(status)) {
				return status;
			}

			break;
		default:
			//
			// Ignore all other descriptors.
			//
			break;
		}
	}

	if (!HaveMemory || !HaveInterrupt) {
		status = STATUS_INSUFFICIENT_RESOURCES;
	}

	return status;
}

NTSTATUS
dwusbDevicePrepareHardware(
	WDFDEVICE Device,
	WDFCMRESLIST ResourcesRaw,
	WDFCMRESLIST ResourcesTranslated
)
{
	NTSTATUS status;
	PDEVICE_CONTEXT deviceContext = DeviceGetContext(Device);
	status = dwusbProbeResources(deviceContext, ResourcesTranslated, ResourcesRaw);

	if (!NT_SUCCESS(status)) {
		return status;
	}

	status = ControllerCreate(Device, &deviceContext->Controller);

	if (NT_SUCCESS(status)) {
		status = RootHubCreate(Device, deviceContext->Controller);
	}

	return status;
}

NTSTATUS
dwusbDeviceD0Entry(
	WDFDEVICE Device,
	WDF_POWER_DEVICE_STATE PreviousState
)
{
	NTSTATUS status;
	PDEVICE_CONTEXT deviceContext = DeviceGetContext(Device);
	UNREFERENCED_PARAMETER(PreviousState);

	status = ControllerInit(deviceContext->Controller);
	if (NT_SUCCESS(status)) {
		status = RootHubInit(deviceContext->Controller);
	}
	return STATUS_SUCCESS;
}

NTSTATUS
dwusbCreateDevice(
    _Inout_ PWDFDEVICE_INIT DeviceInit
    )
/*++

Routine Description:

    Worker routine called to create a device and its software resources.

Arguments:

    DeviceInit - Pointer to an opaque init structure. Memory for this
                    structure will be freed by the framework when the WdfDeviceCreate
                    succeeds. So don't access the structure after that point.

Return Value:

    NTSTATUS

--*/
{
    WDF_OBJECT_ATTRIBUTES deviceAttributes;
	WDF_PNPPOWER_EVENT_CALLBACKS pnpPowerCallbacks;
    PDEVICE_CONTEXT deviceContext;
    WDFDEVICE device;
    NTSTATUS status;

    PAGED_CODE();

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&deviceAttributes, DEVICE_CONTEXT);

	status = UcxInitializeDeviceInit(DeviceInit);

	WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnpPowerCallbacks);
	pnpPowerCallbacks.EvtDevicePrepareHardware = dwusbDevicePrepareHardware;
	//pnpPowerCallbacks.EvtDeviceReleaseHardware = dwusbDeviceReleaseHardware;
	pnpPowerCallbacks.EvtDeviceD0Entry = dwusbDeviceD0Entry;
	//pnpPowerCallbacks.EvtDeviceD0Exit = dwusbDeviceD0Exit;

	WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnpPowerCallbacks);

	WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&deviceAttributes, DEVICE_CONTEXT);

    status = WdfDeviceCreate(&DeviceInit, &deviceAttributes, &device);

    if (NT_SUCCESS(status)) {
		/*WDF_DEVICE_POWER_CAPABILITIES powerCaps;
		WDF_DEVICE_POWER_CAPABILITIES_INIT(&powerCaps);

		powerCaps.DeviceD1 = WdfFalse;
		powerCaps.DeviceD2 = WdfFalse;
		powerCaps.WakeFromD0 = WdfFalse;
		powerCaps.WakeFromD1 = WdfFalse;
		powerCaps.WakeFromD2 = WdfFalse;
		powerCaps.DeviceWake = PowerDeviceD0;
		powerCaps.SystemWake = PowerSystemWorking;

		powerCaps.DeviceState[PowerSystemWorking] = PowerDeviceD0;
		powerCaps.DeviceState[PowerSystemSleeping1] = PowerDeviceD0;
		powerCaps.DeviceState[PowerSystemSleeping2] = PowerDeviceD0;
		powerCaps.DeviceState[PowerSystemSleeping3] = PowerDeviceD0;
		powerCaps.DeviceState[PowerSystemHibernate] = PowerDeviceD3;
		powerCaps.DeviceState[PowerSystemShutdown] = PowerDeviceD3;

		WdfDeviceSetPowerCapabilities(device, &powerCaps);*/


        //
        // Get a pointer to the device context structure that we just associated
        // with the device object. We define this structure in the device.h
        // header file. DeviceGetContext is an inline function generated by
        // using the WDF_DECLARE_CONTEXT_TYPE_WITH_NAME macro in device.h.
        // This function will do the type checking and return the device context.
        // If you pass a wrong object handle it will return NULL and assert if
        // run under framework verifier mode.
        //
        deviceContext = DeviceGetContext(device);

        //
        // Initialize the context.
        //
        deviceContext->Device = device;
    }


	/*if (NT_SUCCESS(status)) {
		//
		// Initialize the I/O Package and any Queues
		//
		status = dwusbQueueInitialize(device);
	}*/

    return status;
}
