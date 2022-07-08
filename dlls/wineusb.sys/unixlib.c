/*
 * libusb backend
 *
 * Copyright 2020 Zebediah Figura
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
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#if 0
#pragma makedep unix
#endif

#include <stdarg.h>
#include <stdbool.h>
#include <stdlib.h>
#include <libusb.h>
#include <pthread.h>
#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winternl.h"
#include "ddk/wdm.h"
#include "ddk/usb.h"
#include "wine/debug.h"
#include "wine/list.h"

#include "unixlib.h"

WINE_DEFAULT_DEBUG_CHANNEL(wineusb);

static libusb_hotplug_callback_handle hotplug_cb_handle;

static bool thread_shutdown;

static pthread_cond_t event_cond = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t event_mutex = PTHREAD_MUTEX_INITIALIZER;

static struct usb_event *usb_events;
static size_t usb_event_count, usb_events_capacity;

static pthread_mutex_t device_mutex = PTHREAD_MUTEX_INITIALIZER;

static struct list device_list = LIST_INIT(device_list);

static bool array_reserve(void **elements, size_t *capacity, size_t count, size_t size)
{
    unsigned int new_capacity, max_capacity;
    void *new_elements;

    if (count <= *capacity)
        return true;

    max_capacity = ~(size_t)0 / size;
    if (count > max_capacity)
        return false;

    new_capacity = max(4, *capacity);
    while (new_capacity < count && new_capacity <= max_capacity / 2)
        new_capacity *= 2;
    if (new_capacity < count)
        new_capacity = max_capacity;

    if (!(new_elements = realloc(*elements, new_capacity * size)))
        return false;

    *elements = new_elements;
    *capacity = new_capacity;

    return true;
}

static void queue_event(const struct usb_event *event)
{
    pthread_mutex_lock(&event_mutex);
    if (array_reserve((void **)&usb_events, &usb_events_capacity, usb_event_count + 1, sizeof(*usb_events)))
        usb_events[usb_event_count++] = *event;
    else
        ERR("Failed to queue event.\n");
    pthread_mutex_unlock(&event_mutex);
    pthread_cond_signal(&event_cond);
}

static NTSTATUS usb_get_event(void *args)
{
    const struct usb_get_event_params *params = args;

    pthread_mutex_lock(&event_mutex);
    while (!usb_event_count)
        pthread_cond_wait(&event_cond, &event_mutex);
    *params->event = usb_events[0];
    if (--usb_event_count)
        memmove(usb_events, usb_events + 1, usb_event_count * sizeof(*usb_events));
    pthread_mutex_unlock(&event_mutex);

    return STATUS_SUCCESS;
}

static void add_usb_device(libusb_device *libusb_device)
{
    struct libusb_device_descriptor device_desc;
    struct unix_device *unix_device;
    struct usb_event usb_event;
    int ret;

    libusb_get_device_descriptor(libusb_device, &device_desc);

    TRACE("Adding new device %p, vendor %04x, product %04x.\n", libusb_device,
            device_desc.idVendor, device_desc.idProduct);

    if (!(unix_device = calloc(1, sizeof(*unix_device))))
        return;

    if ((ret = libusb_open(libusb_device, &unix_device->handle)))
    {
        WARN("Failed to open device: %s\n", libusb_strerror(ret));
        free(unix_device);
        return;
    }
    pthread_mutex_lock(&device_mutex);
    list_add_tail(&device_list, &unix_device->entry);
    pthread_mutex_unlock(&device_mutex);

    usb_event.type = USB_EVENT_ADD_DEVICE;
    usb_event.u.added_device = unix_device;
    queue_event(&usb_event);
}

static void remove_usb_device(libusb_device *libusb_device)
{
    struct unix_device *unix_device;
    struct usb_event usb_event;

    TRACE("Removing device %p.\n", libusb_device);

    LIST_FOR_EACH_ENTRY(unix_device, &device_list, struct unix_device, entry)
    {
        if (libusb_get_device(unix_device->handle) == libusb_device)
        {
            usb_event.type = USB_EVENT_REMOVE_DEVICE;
            usb_event.u.removed_device = unix_device;
            queue_event(&usb_event);
        }
    }
}

static int LIBUSB_CALL hotplug_cb(libusb_context *context, libusb_device *device,
        libusb_hotplug_event event, void *user_data)
{
    if (event == LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED)
        add_usb_device(device);
    else
        remove_usb_device(device);

    return 0;
}

static NTSTATUS usb_main_loop(void *args)
{
    static const struct usb_event shutdown_event = {.type = USB_EVENT_SHUTDOWN};
    int ret;

    TRACE("Starting libusb event thread.\n");

    if ((ret = libusb_hotplug_register_callback(NULL,
            LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED | LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT,
            LIBUSB_HOTPLUG_ENUMERATE, LIBUSB_HOTPLUG_MATCH_ANY, LIBUSB_HOTPLUG_MATCH_ANY,
            LIBUSB_HOTPLUG_MATCH_ANY, hotplug_cb, NULL, &hotplug_cb_handle)))
    {
        ERR("Failed to register callback: %s\n", libusb_strerror(ret));
        return STATUS_UNSUCCESSFUL;
    }

    while (!thread_shutdown)
    {
        if ((ret = libusb_handle_events(NULL)))
            ERR("Error handling events: %s\n", libusb_strerror(ret));
    }

    queue_event(&shutdown_event);

    TRACE("Shutting down libusb event thread.\n");
    return STATUS_SUCCESS;
}

static NTSTATUS usb_exit(void *args)
{
    libusb_hotplug_deregister_callback(NULL, hotplug_cb_handle);
    thread_shutdown = true;
    libusb_interrupt_event_handler(NULL);

    return STATUS_SUCCESS;
}

static NTSTATUS usbd_status_from_libusb(enum libusb_transfer_status status)
{
    switch (status)
    {
        case LIBUSB_TRANSFER_CANCELLED:
            return USBD_STATUS_CANCELED;
        case LIBUSB_TRANSFER_COMPLETED:
            return USBD_STATUS_SUCCESS;
        case LIBUSB_TRANSFER_NO_DEVICE:
            return USBD_STATUS_DEVICE_GONE;
        case LIBUSB_TRANSFER_STALL:
            return USBD_STATUS_ENDPOINT_HALTED;
        case LIBUSB_TRANSFER_TIMED_OUT:
            return USBD_STATUS_TIMEOUT;
        default:
            FIXME("Unhandled status %#x.\n", status);
        case LIBUSB_TRANSFER_ERROR:
            return USBD_STATUS_REQUEST_FAILED;
    }
}

static void LIBUSB_CALL transfer_cb(struct libusb_transfer *transfer)
{
    IRP *irp = transfer->user_data;
    URB *urb = IoGetCurrentIrpStackLocation(irp)->Parameters.Others.Argument1;
    struct usb_event event;

    TRACE("Completing IRP %p, status %#x.\n", irp, transfer->status);

    urb->UrbHeader.Status = usbd_status_from_libusb(transfer->status);

    if (transfer->status == LIBUSB_TRANSFER_COMPLETED)
    {
        switch (urb->UrbHeader.Function)
        {
            case URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER:
                urb->UrbBulkOrInterruptTransfer.TransferBufferLength = transfer->actual_length;
                break;

            case URB_FUNCTION_GET_DESCRIPTOR_FROM_DEVICE:
            {
                struct _URB_CONTROL_DESCRIPTOR_REQUEST *req = &urb->UrbControlDescriptorRequest;
                req->TransferBufferLength = transfer->actual_length;
                memcpy(req->TransferBuffer, libusb_control_transfer_get_data(transfer), transfer->actual_length);
                break;
            }

            case URB_FUNCTION_VENDOR_INTERFACE:
            {
                struct _URB_CONTROL_VENDOR_OR_CLASS_REQUEST *req = &urb->UrbControlVendorClassRequest;
                req->TransferBufferLength = transfer->actual_length;
                if (req->TransferFlags & USBD_TRANSFER_DIRECTION_IN)
                    memcpy(req->TransferBuffer, libusb_control_transfer_get_data(transfer), transfer->actual_length);
                break;
            }

            default:
                ERR("Unexpected function %#x.\n", urb->UrbHeader.Function);
        }
    }

    event.type = USB_EVENT_TRANSFER_COMPLETE;
    event.u.completed_irp = irp;
    queue_event(&event);
}

struct pipe
{
    unsigned char endpoint;
    unsigned char type;
};

static HANDLE make_pipe_handle(unsigned char endpoint, USBD_PIPE_TYPE type)
{
    union
    {
        struct pipe pipe;
        HANDLE handle;
    } u;

    u.pipe.endpoint = endpoint;
    u.pipe.type = type;
    return u.handle;
}

static struct pipe get_pipe(HANDLE handle)
{
    union
    {
        struct pipe pipe;
        HANDLE handle;
    } u;

    u.handle = handle;
    return u.pipe;
}

static NTSTATUS usb_submit_urb(void *args)
{
    const struct usb_submit_urb_params *params = args;
    IRP *irp = params->irp;

    URB *urb = IoGetCurrentIrpStackLocation(irp)->Parameters.Others.Argument1;
    libusb_device_handle *handle = params->device->handle;
    struct libusb_transfer *transfer;
    int ret;

    TRACE("type %#x.\n", urb->UrbHeader.Function);

    switch (urb->UrbHeader.Function)
    {
        case URB_FUNCTION_SYNC_RESET_PIPE_AND_CLEAR_STALL:
        {
            struct _URB_PIPE_REQUEST *req = &urb->UrbPipeRequest;
            struct pipe pipe = get_pipe(req->PipeHandle);

            if ((ret = libusb_clear_halt(handle, pipe.endpoint)) < 0)
                ERR("Failed to clear halt: %s\n", libusb_strerror(ret));

            return STATUS_SUCCESS;
        }

        case URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER:
        {
            struct _URB_BULK_OR_INTERRUPT_TRANSFER *req = &urb->UrbBulkOrInterruptTransfer;
            struct pipe pipe = get_pipe(req->PipeHandle);

            if (req->TransferBufferMDL)
                FIXME("Unhandled MDL output buffer.\n");

            if (!(transfer = libusb_alloc_transfer(0)))
                return STATUS_NO_MEMORY;
            irp->Tail.Overlay.DriverContext[0] = transfer;

            if (pipe.type == UsbdPipeTypeBulk)
            {
                libusb_fill_bulk_transfer(transfer, handle, pipe.endpoint,
                        req->TransferBuffer, req->TransferBufferLength, transfer_cb, irp, 0);
            }
            else if (pipe.type == UsbdPipeTypeInterrupt)
            {
                libusb_fill_interrupt_transfer(transfer, handle, pipe.endpoint,
                        req->TransferBuffer, req->TransferBufferLength, transfer_cb, irp, 0);
            }
            else
            {
                WARN("Invalid pipe type %#x.\n", pipe.type);
                libusb_free_transfer(transfer);
                return USBD_STATUS_INVALID_PIPE_HANDLE;
            }

            transfer->flags = LIBUSB_TRANSFER_FREE_TRANSFER;
            ret = libusb_submit_transfer(transfer);
            if (ret < 0)
                ERR("Failed to submit bulk transfer: %s\n", libusb_strerror(ret));

            return STATUS_PENDING;
        }

        case URB_FUNCTION_GET_DESCRIPTOR_FROM_DEVICE:
        {
            struct _URB_CONTROL_DESCRIPTOR_REQUEST *req = &urb->UrbControlDescriptorRequest;
            unsigned char *buffer;

            if (req->TransferBufferMDL)
                FIXME("Unhandled MDL output buffer.\n");

            if (!(transfer = libusb_alloc_transfer(0)))
                return STATUS_NO_MEMORY;
            irp->Tail.Overlay.DriverContext[0] = transfer;

            if (!(buffer = malloc(sizeof(struct libusb_control_setup) + req->TransferBufferLength)))
            {
                libusb_free_transfer(transfer);
                return STATUS_NO_MEMORY;
            }

            libusb_fill_control_setup(buffer,
                    LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_STANDARD | LIBUSB_RECIPIENT_DEVICE,
                    LIBUSB_REQUEST_GET_DESCRIPTOR, (req->DescriptorType << 8) | req->Index,
                    req->LanguageId, req->TransferBufferLength);
            libusb_fill_control_transfer(transfer, handle, buffer, transfer_cb, irp, 0);
            transfer->flags = LIBUSB_TRANSFER_FREE_BUFFER | LIBUSB_TRANSFER_FREE_TRANSFER;
            ret = libusb_submit_transfer(transfer);
            if (ret < 0)
                ERR("Failed to submit GET_DESCRIPTOR transfer: %s\n", libusb_strerror(ret));

            return STATUS_PENDING;
        }

        case URB_FUNCTION_SELECT_CONFIGURATION:
        {
            struct _URB_SELECT_CONFIGURATION *req = &urb->UrbSelectConfiguration;
            ULONG i;

            /* FIXME: In theory, we'd call libusb_set_configuration() here, but
             * the CASIO FX-9750GII (which has only one configuration) goes into
             * an error state if it receives a SET_CONFIGURATION request. Maybe
             * we should skip setting that if and only if the configuration is
             * already active? */

            for (i = 0; i < req->Interface.NumberOfPipes; ++i)
            {
                USBD_PIPE_INFORMATION *pipe = &req->Interface.Pipes[i];
                pipe->PipeHandle = make_pipe_handle(pipe->EndpointAddress, pipe->PipeType);
            }

            return STATUS_SUCCESS;
        }

        case URB_FUNCTION_VENDOR_INTERFACE:
        {
            struct _URB_CONTROL_VENDOR_OR_CLASS_REQUEST *req = &urb->UrbControlVendorClassRequest;
            uint8_t req_type = LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_INTERFACE;
            unsigned char *buffer;

            if (req->TransferFlags & USBD_TRANSFER_DIRECTION_IN)
                req_type |= LIBUSB_ENDPOINT_IN;
            if (req->TransferFlags & ~USBD_TRANSFER_DIRECTION_IN)
                FIXME("Unhandled flags %#x.\n", req->TransferFlags);

            if (req->TransferBufferMDL)
                FIXME("Unhandled MDL output buffer.\n");

            if (!(transfer = libusb_alloc_transfer(0)))
                return STATUS_NO_MEMORY;
            irp->Tail.Overlay.DriverContext[0] = transfer;

            if (!(buffer = malloc(sizeof(struct libusb_control_setup) + req->TransferBufferLength)))
            {
                libusb_free_transfer(transfer);
                return STATUS_NO_MEMORY;
            }

            libusb_fill_control_setup(buffer, req_type, req->Request,
                    req->Value, req->Index, req->TransferBufferLength);
            if (!(req->TransferFlags & USBD_TRANSFER_DIRECTION_IN))
                memcpy(buffer + LIBUSB_CONTROL_SETUP_SIZE, req->TransferBuffer, req->TransferBufferLength);
            libusb_fill_control_transfer(transfer, handle, buffer, transfer_cb, irp, 0);
            transfer->flags = LIBUSB_TRANSFER_FREE_BUFFER | LIBUSB_TRANSFER_FREE_TRANSFER;
            ret = libusb_submit_transfer(transfer);
            if (ret < 0)
                ERR("Failed to submit vendor-specific interface transfer: %s\n", libusb_strerror(ret));

            return STATUS_PENDING;
        }

        default:
            FIXME("Unhandled function %#x.\n", urb->UrbHeader.Function);
    }

    return STATUS_NOT_IMPLEMENTED;
}

static NTSTATUS usb_cancel_transfer(void *args)
{
    const struct usb_cancel_transfer_params *params = args;
    int ret;

    if ((ret = libusb_cancel_transfer(params->transfer)) < 0)
        ERR("Failed to cancel transfer: %s\n", libusb_strerror(ret));

    return STATUS_SUCCESS;
}

static NTSTATUS usb_destroy_device(void *args)
{
    const struct usb_destroy_device_params *params = args;
    struct unix_device *device = params->device;

    pthread_mutex_lock(&device_mutex);
    libusb_close(device->handle);
    list_remove(&device->entry);
    pthread_mutex_unlock(&device_mutex);
    free(device);

    return STATUS_SUCCESS;
}

const unixlib_entry_t __wine_unix_call_funcs[] =
{
#define X(name) [unix_ ## name] = name
    X(usb_main_loop),
    X(usb_exit),
    X(usb_get_event),
    X(usb_submit_urb),
    X(usb_cancel_transfer),
    X(usb_destroy_device),
};