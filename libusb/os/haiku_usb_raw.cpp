/*
 * Haiku Backend for libusb
 * Copyright © 2014 Akshay Jaggi <akshay1994.leo@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */


#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <new>
#include <vector>

#include "haiku_usb.h"

USBRoster gUsbRoster;
int32 gInitCount = 0;

static int haiku_get_config_descriptor(struct libusb_device *, uint8_t,
    void *, size_t);

static int
haiku_init(struct libusb_context *ctx)
{
	UNUSED(ctx);
	if (atomic_add(&gInitCount, 1) == 0)
		return gUsbRoster.Start();
	return LIBUSB_SUCCESS;
}

static void
haiku_exit(struct libusb_context *ctx)
{
	UNUSED(ctx);
	if (atomic_add(&gInitCount, -1) == 1)
		gUsbRoster.Stop();
}

static int
haiku_open(struct libusb_device_handle *dev_handle)
{
	USBDevice *dev = *((USBDevice **)usbi_get_device_priv(dev_handle->dev));
	USBDeviceHandle *handle = new(std::nothrow) USBDeviceHandle(dev);
	if (handle == NULL)
		return LIBUSB_ERROR_NO_MEM;
	if (handle->InitCheck() == false) {
		delete handle;
		return LIBUSB_ERROR_NO_DEVICE;
	}
	*((USBDeviceHandle **)usbi_get_device_handle_priv(dev_handle)) = handle;
	return LIBUSB_SUCCESS;
}

static void
haiku_close(struct libusb_device_handle *dev_handle)
{
	USBDeviceHandle **pHandle = (USBDeviceHandle **)usbi_get_device_handle_priv(dev_handle);
	USBDeviceHandle *handle = *pHandle;
	if (handle == NULL)
		return;
	delete handle;
	*pHandle = NULL;
}

static int
haiku_get_active_config_descriptor(struct libusb_device *device, void *buffer, size_t len)
{
	USBDevice *dev = *((USBDevice **)usbi_get_device_priv(device));
	return haiku_get_config_descriptor(device, dev->ActiveConfigurationIndex(), buffer, len);
}

static int
haiku_get_config_descriptor(struct libusb_device *device, uint8_t config_index, void *buffer, size_t len)
{
	USBDevice *dev = *((USBDevice **)usbi_get_device_priv(device));
	const usb_configuration_descriptor *config = dev->ConfigurationDescriptor(config_index);
	if (config == NULL) {
		usbi_err(DEVICE_CTX(device), "failed getting configuration descriptor");
		return LIBUSB_ERROR_IO;
	}
	if (len > config->total_length) {
		len = config->total_length;
	}
	memcpy(buffer, config, len);
	return len;
}

static int
haiku_set_configuration(struct libusb_device_handle *dev_handle, int config)
{
	USBDeviceHandle *handle= *((USBDeviceHandle **)usbi_get_device_handle_priv(dev_handle));
	if (config <= 0)
		return LIBUSB_ERROR_NOT_SUPPORTED;	// cannot unconfigure
	return handle->SetConfiguration((uint8)config);
}

static int
haiku_claim_interface(struct libusb_device_handle *dev_handle, uint8_t interface_number)
{
	USBDeviceHandle *handle = *((USBDeviceHandle **)usbi_get_device_handle_priv(dev_handle));
	return handle->ClaimInterface(interface_number);
}

static int
haiku_set_altsetting(struct libusb_device_handle *dev_handle, uint8_t interface_number, uint8_t altsetting)
{
	USBDeviceHandle *handle = *((USBDeviceHandle **)usbi_get_device_handle_priv(dev_handle));
	return handle->SetAltSetting(interface_number, altsetting);
}

static int
haiku_clear_halt(struct libusb_device_handle *dev_handle, unsigned char endpoint)
{
	USBDeviceHandle *handle = *((USBDeviceHandle **)usbi_get_device_handle_priv(dev_handle));
	return handle->ClearHalt(endpoint);
}

static int
haiku_release_interface(struct libusb_device_handle *dev_handle, uint8_t interface_number)
{
	USBDeviceHandle *handle = *((USBDeviceHandle **)usbi_get_device_handle_priv(dev_handle));
	haiku_set_altsetting(dev_handle, interface_number, 0);
	return handle->ReleaseInterface(interface_number);
}

static int
haiku_submit_transfer(struct usbi_transfer *itransfer)
{
	struct libusb_transfer *fLibusbTransfer = USBI_TRANSFER_TO_LIBUSB_TRANSFER(itransfer);
	USBDeviceHandle *fDeviceHandle = *((USBDeviceHandle **)usbi_get_device_handle_priv(fLibusbTransfer->dev_handle));
	return fDeviceHandle->SubmitTransfer(itransfer);
}

static int
haiku_cancel_transfer(struct usbi_transfer *itransfer)
{
	struct libusb_transfer *fLibusbTransfer = USBI_TRANSFER_TO_LIBUSB_TRANSFER(itransfer);
	USBDeviceHandle *fDeviceHandle = *((USBDeviceHandle **)usbi_get_device_handle_priv(fLibusbTransfer->dev_handle));
	return fDeviceHandle->CancelTransfer(*((USBTransfer **)usbi_get_transfer_priv(itransfer)));
}

static int
haiku_handle_transfer_completion(struct usbi_transfer *itransfer)
{
	USBTransfer **pTransfer = (USBTransfer **)usbi_get_transfer_priv(itransfer);
	USBTransfer *transfer = *pTransfer;

	usbi_mutex_lock(&itransfer->lock);
	if (transfer->IsCancelled()) {
		delete transfer;
		*pTransfer = NULL;
		usbi_mutex_unlock(&itransfer->lock);
		if (itransfer->transferred < 0)
			itransfer->transferred = 0;
		return usbi_handle_transfer_cancellation(itransfer);
	}
	libusb_transfer_status status = LIBUSB_TRANSFER_COMPLETED;
	if (itransfer->transferred < 0) {
		usbi_err(ITRANSFER_CTX(itransfer), "error in transfer");
		status = LIBUSB_TRANSFER_ERROR;
		itransfer->transferred = 0;
	}
	delete transfer;
	*pTransfer = NULL;
	usbi_mutex_unlock(&itransfer->lock);
	return usbi_handle_transfer_completion(itransfer, status);
}

const struct usbi_os_backend usbi_backend = {
	/*.name =*/ "Haiku usbfs",
	/*.caps =*/ 0,
	/*.init =*/ haiku_init,
	/*.exit =*/ haiku_exit,
	/*.set_option =*/ NULL,
	/*.get_device_list =*/ NULL,
	/*.hotplug_poll =*/ NULL,
	/*.wrap_sys_device =*/ NULL,
	/*.get_dev_path =*/ NULL,
	/*.open =*/ haiku_open,
	/*.close =*/ haiku_close,

	/*.get_active_config_descriptor =*/ haiku_get_active_config_descriptor,
	/*.get_config_descriptor =*/ haiku_get_config_descriptor,
	/*.get_config_descriptor_by_value =*/ NULL,

	/*.get_configuration =*/ NULL,
	/*.set_configuration =*/ haiku_set_configuration,

	/*.claim_interface =*/ haiku_claim_interface,
	/*.release_interface =*/ haiku_release_interface,
	/*.set_interface_altsetting =*/ haiku_set_altsetting,

	/*.clear_halt =*/ haiku_clear_halt,
	/*.reset_device =*/ NULL,

	/*.alloc_streams =*/ NULL,
	/*.free_streams =*/ NULL,

	/*.dev_mem_alloc =*/ NULL,
	/*.dev_mem_free =*/ NULL,

	/*.kernel_driver_active =*/ NULL,
	/*.detach_kernel_driver =*/ NULL,
	/*.attach_kernel_driver =*/ NULL,

	/*.destroy_device =*/ NULL,

	/*.submit_transfer =*/ haiku_submit_transfer,
	/*.cancel_transfer =*/ haiku_cancel_transfer,
	/*.clear_transfer_priv =*/ NULL,

	/*.handle_events =*/ NULL,
	/*.handle_transfer_completion =*/ haiku_handle_transfer_completion,

	/*.context_priv_size =*/ 0,
	/*.device_priv_size =*/ sizeof(USBDevice *),
	/*.device_handle_priv_size =*/ sizeof(USBDeviceHandle *),
	/*.transfer_priv_size =*/ sizeof(USBTransfer *),
};
