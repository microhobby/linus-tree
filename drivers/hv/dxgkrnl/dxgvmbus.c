// SPDX-License-Identifier: GPL-2.0

/*
 * Copyright (c) 2019, Microsoft Corporation.
 *
 * Author:
 *   Iouri Tarassov <iourit@linux.microsoft.com>
 *
 * Dxgkrnl Graphics Driver
 * VM bus interface implementation
 *
 */

#include <linux/kernel.h>
#include <linux/completion.h>
#include <linux/slab.h>
#include <linux/eventfd.h>
#include <linux/hyperv.h>
#include <linux/mman.h>
#include <linux/delay.h>
#include <linux/pagemap.h>
#include "dxgkrnl.h"
#include "dxgvmbus.h"

#undef pr_fmt
#define pr_fmt(fmt)	"dxgk:err: " fmt

#define RING_BUFSIZE (256 * 1024)

/*
 * The structure is used to track VM bus packets, waiting for completion.
 */
struct dxgvmbuspacket {
	struct list_head packet_list_entry;
	u64 request_id;
	struct completion wait;
	void *buffer;
	u32 buffer_length;
	int status;
};

struct dxgvmb_ext_header {
	/* Offset from the start of the message to DXGKVMB_COMMAND_BASE */
	u32		command_offset;
	u32		reserved;
	struct winluid	vgpu_luid;
};

#define VMBUSMESSAGEONSTACK	64

struct dxgvmbusmsg {
/* Points to the allocated buffer */
	struct dxgvmb_ext_header	*hdr;
/* Points to dxgkvmb_command_vm_to_host or dxgkvmb_command_vgpu_to_host */
	void				*msg;
/* The vm bus channel, used to pass the message to the host */
	struct dxgvmbuschannel		*channel;
/* Message size in bytes including the header and the payload */
	u32				size;
/* Buffer used for small messages */
	char				msg_on_stack[VMBUSMESSAGEONSTACK];
};

struct dxgvmbusmsgres {
/* Points to the allocated buffer */
	struct dxgvmb_ext_header	*hdr;
/* Points to dxgkvmb_command_vm_to_host or dxgkvmb_command_vgpu_to_host */
	void				*msg;
/* The vm bus channel, used to pass the message to the host */
	struct dxgvmbuschannel		*channel;
/* Message size in bytes including the header, the payload and the result */
	u32				size;
/* Result buffer size in bytes */
	u32				res_size;
/* Points to the result within the allocated buffer */
	void				*res;
};

static int init_message(struct dxgvmbusmsg *msg, struct dxgadapter *adapter,
			struct dxgprocess *process, u32 size)
{
	bool use_ext_header = dxgglobal->vmbus_ver >=
			      DXGK_VMBUS_INTERFACE_VERSION;

	if (use_ext_header)
		size += sizeof(struct dxgvmb_ext_header);
	msg->size = size;
	if (size <= VMBUSMESSAGEONSTACK) {
		msg->hdr = (void *)msg->msg_on_stack;
		memset(msg->hdr, 0, size);
	} else {
		msg->hdr = vzalloc(size);
		if (msg->hdr == NULL)
			return -ENOMEM;
	}
	if (use_ext_header) {
		msg->msg = (char *)&msg->hdr[1];
		msg->hdr->command_offset = sizeof(msg->hdr[0]);
		if (adapter)
			msg->hdr->vgpu_luid = adapter->host_vgpu_luid;
	} else {
		msg->msg = (char *)msg->hdr;
	}
	if (adapter && !dxgglobal->async_msg_enabled)
		msg->channel = &adapter->channel;
	else
		msg->channel = &dxgglobal->channel;
	return 0;
}

static int init_message_res(struct dxgvmbusmsgres *msg,
			    struct dxgadapter *adapter,
			    struct dxgprocess *process,
			    u32 size,
			    u32 result_size)
{
	bool use_ext_header = dxgglobal->vmbus_ver >=
			      DXGK_VMBUS_INTERFACE_VERSION;

	if (use_ext_header)
		size += sizeof(struct dxgvmb_ext_header);
	msg->size = size;
	msg->res_size += (result_size + 7) & ~7;
	size += msg->res_size;
	msg->hdr = vzalloc(size);
	if (msg->hdr == NULL) {
		pr_err("Failed to allocate VM bus message: %d", size);
		return -ENOMEM;
	}
	if (use_ext_header) {
		msg->msg = (char *)&msg->hdr[1];
		msg->hdr->command_offset = sizeof(msg->hdr[0]);
		msg->hdr->vgpu_luid = adapter->host_vgpu_luid;
	} else {
		msg->msg = (char *)msg->hdr;
	}
	msg->res = (char *)msg->hdr + msg->size;
	if (dxgglobal->async_msg_enabled)
		msg->channel = &dxgglobal->channel;
	else
		msg->channel = &adapter->channel;
	return 0;
}

static void free_message(struct dxgvmbusmsg *msg, struct dxgprocess *process)
{
	if (msg->hdr && (char *)msg->hdr != msg->msg_on_stack)
		vfree(msg->hdr);
}

int ntstatus2int(struct ntstatus status)
{
	if (NT_SUCCESS(status))
		return (int)status.v;
	switch (status.v) {
	case STATUS_OBJECT_NAME_COLLISION:
		return -EEXIST;
	case STATUS_NO_MEMORY:
		return -ENOMEM;
	case STATUS_INVALID_PARAMETER:
		return -EINVAL;
	case STATUS_OBJECT_NAME_INVALID:
	case STATUS_OBJECT_NAME_NOT_FOUND:
		return -ENOENT;
	case STATUS_TIMEOUT:
		return -EAGAIN;
	case STATUS_BUFFER_TOO_SMALL:
		return -EOVERFLOW;
	case STATUS_DEVICE_REMOVED:
		return -ENODEV;
	case STATUS_ACCESS_DENIED:
		return -EACCES;
	case STATUS_NOT_SUPPORTED:
		return -EPERM;
	case STATUS_ILLEGAL_INSTRUCTION:
		return -EOPNOTSUPP;
	case STATUS_INVALID_HANDLE:
		return -EBADF;
	case STATUS_GRAPHICS_ALLOCATION_BUSY:
		return -EINPROGRESS;
	case STATUS_OBJECT_TYPE_MISMATCH:
		return -EPROTOTYPE;
	case STATUS_NOT_IMPLEMENTED:
		return -EPERM;
	default:
		return -EINVAL;
	}
}

int dxgvmbuschannel_init(struct dxgvmbuschannel *ch, struct hv_device *hdev)
{
	int ret;

	ch->hdev = hdev;
	spin_lock_init(&ch->packet_list_mutex);
	INIT_LIST_HEAD(&ch->packet_list_head);
	atomic64_set(&ch->packet_request_id, 0);

	ch->packet_cache = kmem_cache_create("DXGK packet cache",
					     sizeof(struct dxgvmbuspacket), 0,
					     0, NULL);
	if (ch->packet_cache == NULL) {
		pr_err("packet_cache alloc failed");
		ret = -ENOMEM;
		goto cleanup;
	}

	hdev->channel->max_pkt_size = DXG_MAX_VM_BUS_PACKET_SIZE;
	ret = vmbus_open(hdev->channel, RING_BUFSIZE, RING_BUFSIZE,
			 NULL, 0, dxgvmbuschannel_receive, ch);
	if (ret) {
		pr_err("vmbus_open failed: %d", ret);
		goto cleanup;
	}

	ch->channel = hdev->channel;

cleanup:

	return ret;
}

void dxgvmbuschannel_destroy(struct dxgvmbuschannel *ch)
{
	kmem_cache_destroy(ch->packet_cache);
	ch->packet_cache = NULL;

	if (ch->channel) {
		vmbus_close(ch->channel);
		ch->channel = NULL;
	}
}

static inline void command_vm_to_host_init0(struct dxgkvmb_command_vm_to_host
					    *command)
{
	command->command_type = DXGK_VMBCOMMAND_INVALID_VM_TO_HOST;
	command->process.v = 0;
	command->command_id = 0;
	command->channel_type = DXGKVMB_VM_TO_HOST;
}

static inline void command_vm_to_host_init1(struct dxgkvmb_command_vm_to_host
					    *command,
					    enum dxgkvmb_commandtype_global
					    type)
{
	command->command_type = type;
	command->process.v = 0;
	command->command_id = 0;
	command->channel_type = DXGKVMB_VM_TO_HOST;
}

void process_inband_packet(struct dxgvmbuschannel *channel,
			   struct vmpacket_descriptor *desc)
{
	u32 packet_length = hv_pkt_datalen(desc);

	if (channel->adapter == NULL) {
		if (packet_length < sizeof(struct dxgkvmb_command_host_to_vm)) {
			pr_err("Invalid global packet");
		} else {
			/*
			 *Placeholder
			 */
		}
	} else {
		pr_err("Unexpected packet for adapter channel");
	}
}

void process_completion_packet(struct dxgvmbuschannel *channel,
			       struct vmpacket_descriptor *desc)
{
	struct dxgvmbuspacket *packet = NULL;
	struct dxgvmbuspacket *entry;
	u32 packet_length = hv_pkt_datalen(desc);
	unsigned long flags;

	spin_lock_irqsave(&channel->packet_list_mutex, flags);
	list_for_each_entry(entry, &channel->packet_list_head,
			    packet_list_entry) {
		if (desc->trans_id == entry->request_id) {
			packet = entry;
			list_del(&packet->packet_list_entry);
			break;
		}
	}
	spin_unlock_irqrestore(&channel->packet_list_mutex, flags);
	if (packet) {
		if (packet->buffer_length) {
			if (packet_length < packet->buffer_length) {
				dev_dbg(dxgglobaldev, "invalid size %d Expected:%d",
					    packet_length,
					    packet->buffer_length);
				packet->status = -EOVERFLOW;
			} else {
				memcpy(packet->buffer, hv_pkt_data(desc),
				       packet->buffer_length);
			}
		}
		complete(&packet->wait);
	} else {
		pr_err("did not find packet to complete");
	}
}

/* Receive callback for messages from the host */
void dxgvmbuschannel_receive(void *ctx)
{
	struct dxgvmbuschannel *channel = ctx;
	struct vmpacket_descriptor *desc;
	u32 packet_length = 0;

	dev_dbg(dxgglobaldev, "%s %p", __func__, channel->adapter);
	foreach_vmbus_pkt(desc, channel->channel) {
		packet_length = hv_pkt_datalen(desc);
		dev_dbg(dxgglobaldev, "next packet (id, size, type): %llu %d %d",
			desc->trans_id, packet_length, desc->type);
		if (desc->type == VM_PKT_COMP) {
			process_completion_packet(channel, desc);
		} else {
			if (desc->type != VM_PKT_DATA_INBAND)
				pr_err("unexpected packet type");
			else
				process_inband_packet(channel, desc);
		}
	}
}

int dxgvmb_send_sync_msg(struct dxgvmbuschannel *channel,
			 void *command,
			 u32 cmd_size,
			 void *result,
			 u32 result_size)
{
	int ret;
	struct dxgvmbuspacket *packet = NULL;
	struct dxgkvmb_command_vm_to_host *cmd1;
	struct dxgkvmb_command_vgpu_to_host *cmd2;

	if (cmd_size > DXG_MAX_VM_BUS_PACKET_SIZE ||
	    result_size > DXG_MAX_VM_BUS_PACKET_SIZE) {
		pr_err("%s invalid data size", __func__);
		return -EINVAL;
	}

	packet = kmem_cache_alloc(channel->packet_cache, 0);
	if (packet == NULL) {
		pr_err("kmem_cache_alloc failed");
		return -ENOMEM;
	}

	if (channel->adapter == NULL) {
		cmd1 = command;
		dev_dbg(dxgglobaldev, "send_sync_msg global: %d %p %d %d",
			cmd1->command_type, command, cmd_size, result_size);
	} else {
		cmd2 = command;
		dev_dbg(dxgglobaldev, "send_sync_msg adapter: %d %p %d %d",
			cmd2->command_type, command, cmd_size, result_size);
	}

	packet->request_id = atomic64_inc_return(&channel->packet_request_id);
	init_completion(&packet->wait);
	packet->buffer = result;
	packet->buffer_length = result_size;
	packet->status = 0;
	spin_lock_irq(&channel->packet_list_mutex);
	list_add_tail(&packet->packet_list_entry, &channel->packet_list_head);
	spin_unlock_irq(&channel->packet_list_mutex);

	ret = vmbus_sendpacket(channel->channel, command, cmd_size,
			       packet->request_id, VM_PKT_DATA_INBAND,
			       VMBUS_DATA_PACKET_FLAG_COMPLETION_REQUESTED);
	if (ret) {
		pr_err("vmbus_sendpacket failed: %x", ret);
		spin_lock_irq(&channel->packet_list_mutex);
		list_del(&packet->packet_list_entry);
		spin_unlock_irq(&channel->packet_list_mutex);
		goto cleanup;
	}

	dev_dbg(dxgglobaldev, "waiting completion: %llu", packet->request_id);
	wait_for_completion(&packet->wait);
	dev_dbg(dxgglobaldev, "completion done: %llu %x",
		packet->request_id, packet->status);
	ret = packet->status;

cleanup:

	kmem_cache_free(channel->packet_cache, packet);
	if (ret < 0)
		dev_dbg(dxgglobaldev, "%s failed: %x", __func__, ret);
	return ret;
}

int dxgvmb_send_async_msg(struct dxgvmbuschannel *channel,
			  void *command,
			  u32 cmd_size)
{
	int ret;
	int try_count = 0;

	if (cmd_size > DXG_MAX_VM_BUS_PACKET_SIZE) {
		pr_err("%s invalid data size", __func__);
		return -EINVAL;
	}

	if (channel->adapter) {
		pr_err("Async messages should be sent to the global channel");
		return -EINVAL;
	}

	do {
		ret = vmbus_sendpacket(channel->channel, command, cmd_size,
				0, VM_PKT_DATA_INBAND, 0);
		/*
		 * -EAGAIN is returned when the VM bus ring buffer if full.
		 * Wait 2ms to allow the host to process messages and try again.
		 */
		if (ret == -EAGAIN) {
			usleep_range(1000, 2000);
			try_count++;
		}
	} while (ret == -EAGAIN && try_count < 5000);
	if (ret < 0)
		pr_err("vmbus_sendpacket failed: %x", ret);

	return ret;
}

static int
dxgvmb_send_sync_msg_ntstatus(struct dxgvmbuschannel *channel,
			      void *command, u32 cmd_size)
{
	struct ntstatus status;
	int ret;

	ret = dxgvmb_send_sync_msg(channel, command, cmd_size,
				   &status, sizeof(status));
	if (ret >= 0)
		ret = ntstatus2int(status);
	return ret;
}

int dxg_unmap_iospace(void *va, u32 size)
{
	int ret = 0;

	dev_dbg(dxgglobaldev, "%s %p %x", __func__, va, size);

	/*
	 * When an app calls exit(), dxgkrnl is called to close the device
	 * with current->mm equal to NULL.
	 */
	if (current->mm) {
		ret = vm_munmap((unsigned long)va, size);
		if (ret) {
			pr_err("vm_munmap failed %d", ret);
			return -ENOTRECOVERABLE;
		}
	}
	return 0;
}

/*
 * Global messages to the host
 */

int dxgvmb_send_set_iospace_region(u64 start, u64 len,
	struct vmbus_gpadl *shared_mem_gpadl)
{
	int ret;
	struct dxgkvmb_command_setiospaceregion *command;
	struct dxgvmbusmsg msg;

	ret = init_message(&msg, NULL, NULL, sizeof(*command));
	if (ret)
		return ret;
	command = (void *)msg.msg;

	ret = dxgglobal_acquire_channel_lock();
	if (ret < 0)
		goto cleanup;

	command_vm_to_host_init1(&command->hdr,
				 DXGK_VMBCOMMAND_SETIOSPACEREGION);
	command->start = start;
	command->length = len;
	if (command->shared_page_gpadl)
		command->shared_page_gpadl = shared_mem_gpadl->gpadl_handle;
	ret = dxgvmb_send_sync_msg_ntstatus(&dxgglobal->channel, msg.hdr,
					    msg.size);
	if (ret < 0)
		pr_err("send_set_iospace_region failed %x", ret);

	dxgglobal_release_channel_lock();
cleanup:
	free_message(&msg, NULL);
	if (ret)
		dev_dbg(dxgglobaldev, "err: %s %d", __func__, ret);
	return ret;
}

int dxgvmb_send_create_process(struct dxgprocess *process)
{
	int ret;
	struct dxgkvmb_command_createprocess *command;
	struct dxgkvmb_command_createprocess_return result = { 0 };
	struct dxgvmbusmsg msg;
	char s[WIN_MAX_PATH];
	int i;

	ret = init_message(&msg, NULL, process, sizeof(*command));
	if (ret)
		return ret;
	command = (void *)msg.msg;

	ret = dxgglobal_acquire_channel_lock();
	if (ret < 0)
		goto cleanup;

	command_vm_to_host_init1(&command->hdr, DXGK_VMBCOMMAND_CREATEPROCESS);
	command->process = process;
	command->process_id = process->process->pid;
	command->linux_process = 1;
	s[0] = 0;
	__get_task_comm(s, WIN_MAX_PATH, process->process);
	for (i = 0; i < WIN_MAX_PATH; i++) {
		command->process_name[i] = s[i];
		if (s[i] == 0)
			break;
	}

	ret = dxgvmb_send_sync_msg(&dxgglobal->channel, msg.hdr, msg.size,
				   &result, sizeof(result));
	if (ret < 0) {
		pr_err("create_process failed %d", ret);
	} else if (result.hprocess.v == 0) {
		pr_err("create_process returned 0 handle");
		ret = -ENOTRECOVERABLE;
	} else {
		process->host_handle = result.hprocess;
		dev_dbg(dxgglobaldev, "create_process returned %x",
			    process->host_handle.v);
	}

	dxgglobal_release_channel_lock();

cleanup:
	free_message(&msg, process);
	if (ret)
		dev_dbg(dxgglobaldev, "err: %s %d", __func__, ret);
	return ret;
}

int dxgvmb_send_destroy_process(struct d3dkmthandle process)
{
	int ret;
	struct dxgkvmb_command_destroyprocess *command;
	struct dxgvmbusmsg msg;

	ret = init_message(&msg, NULL, NULL, sizeof(*command));
	if (ret)
		return ret;
	command = (void *)msg.msg;

	ret = dxgglobal_acquire_channel_lock();
	if (ret < 0)
		goto cleanup;
	command_vm_to_host_init2(&command->hdr, DXGK_VMBCOMMAND_DESTROYPROCESS,
				 process);
	ret = dxgvmb_send_sync_msg_ntstatus(&dxgglobal->channel,
					    msg.hdr, msg.size);
	dxgglobal_release_channel_lock();

cleanup:
	free_message(&msg, NULL);
	if (ret)
		dev_dbg(dxgglobaldev, "err: %s %d", __func__, ret);
	return ret;
}

/*
 * Virtual GPU messages to the host
 */

int dxgvmb_send_open_adapter(struct dxgadapter *adapter)
{
	int ret;
	struct dxgkvmb_command_openadapter *command;
	struct dxgkvmb_command_openadapter_return result = { };
	struct dxgvmbusmsg msg;

	ret = init_message(&msg, adapter, NULL, sizeof(*command));
	if (ret)
		return ret;
	command = (void *)msg.msg;

	command_vgpu_to_host_init1(&command->hdr, DXGK_VMBCOMMAND_OPENADAPTER);
	command->vmbus_interface_version = dxgglobal->vmbus_ver;
	command->vmbus_last_compatible_interface_version =
	    DXGK_VMBUS_LAST_COMPATIBLE_INTERFACE_VERSION;

	ret = dxgvmb_send_sync_msg(msg.channel, msg.hdr, msg.size,
				   &result, sizeof(result));
	if (ret < 0)
		goto cleanup;

	ret = ntstatus2int(result.status);
	adapter->host_handle = result.host_adapter_handle;

cleanup:
	free_message(&msg, NULL);
	if (ret)
		dev_dbg(dxgglobaldev, "err: %s %d", __func__, ret);
	return ret;
}

int dxgvmb_send_close_adapter(struct dxgadapter *adapter)
{
	int ret;
	struct dxgkvmb_command_closeadapter *command;
	struct dxgvmbusmsg msg;

	ret = init_message(&msg, adapter, NULL, sizeof(*command));
	if (ret)
		return ret;
	command = (void *)msg.msg;

	command_vgpu_to_host_init1(&command->hdr, DXGK_VMBCOMMAND_CLOSEADAPTER);
	command->host_handle = adapter->host_handle;

	ret = dxgvmb_send_sync_msg(msg.channel, msg.hdr, msg.size,
				   NULL, 0);
	free_message(&msg, NULL);
	if (ret)
		dev_dbg(dxgglobaldev, "err: %s %d", __func__, ret);
	return ret;
}

int dxgvmb_send_get_internal_adapter_info(struct dxgadapter *adapter)
{
	int ret;
	struct dxgkvmb_command_getinternaladapterinfo *command;
	struct dxgkvmb_command_getinternaladapterinfo_return result = { };
	struct dxgvmbusmsg msg;
	u32 result_size = sizeof(result);

	ret = init_message(&msg, adapter, NULL, sizeof(*command));
	if (ret)
		return ret;
	command = (void *)msg.msg;

	command_vgpu_to_host_init1(&command->hdr,
				   DXGK_VMBCOMMAND_GETINTERNALADAPTERINFO);
	if (dxgglobal->vmbus_ver < DXGK_VMBUS_INTERFACE_VERSION)
		result_size -= sizeof(struct winluid);

	ret = dxgvmb_send_sync_msg(msg.channel, msg.hdr, msg.size,
				   &result, result_size);
	if (ret >= 0) {
		adapter->host_adapter_luid = result.host_adapter_luid;
		adapter->host_vgpu_luid = result.host_vgpu_luid;
		wcsncpy(adapter->device_description, result.device_description,
			sizeof(adapter->device_description) / sizeof(u16));
		wcsncpy(adapter->device_instance_id, result.device_instance_id,
			sizeof(adapter->device_instance_id) / sizeof(u16));
		dxgglobal->async_msg_enabled = result.async_msg_enabled != 0;
	}
	free_message(&msg, NULL);
	if (ret)
		dev_dbg(dxgglobaldev, "err: %s %d", __func__, ret);
	return ret;
}

struct d3dkmthandle dxgvmb_send_create_device(struct dxgadapter *adapter,
					struct dxgprocess *process,
					struct d3dkmt_createdevice *args)
{
	int ret;
	struct dxgkvmb_command_createdevice *command;
	struct dxgkvmb_command_createdevice_return result = { };
	struct dxgvmbusmsg msg;

	ret = init_message(&msg, adapter, process, sizeof(*command));
	if (ret)
		goto cleanup;
	command = (void *)msg.msg;

	command_vgpu_to_host_init2(&command->hdr, DXGK_VMBCOMMAND_CREATEDEVICE,
				   process->host_handle);
	command->flags = args->flags;
	command->error_code = &dxgglobal->device_state_counter;

	ret = dxgvmb_send_sync_msg(msg.channel, msg.hdr, msg.size,
				   &result, sizeof(result));
	if (ret < 0)
		result.device.v = 0;
	free_message(&msg, process);
cleanup:
	if (ret)
		dev_dbg(dxgglobaldev, "err: %s %d", __func__, ret);
	return result.device;
}

int dxgvmb_send_destroy_device(struct dxgadapter *adapter,
			       struct dxgprocess *process,
			       struct d3dkmthandle h)
{
	int ret;
	struct dxgkvmb_command_destroydevice *command;
	struct dxgvmbusmsg msg = {.hdr = NULL};

	ret = init_message(&msg, adapter, process, sizeof(*command));
	if (ret)
		goto cleanup;
	command = (void *)msg.msg;

	command_vgpu_to_host_init2(&command->hdr, DXGK_VMBCOMMAND_DESTROYDEVICE,
				   process->host_handle);
	command->device = h;

	ret = dxgvmb_send_sync_msg_ntstatus(msg.channel, msg.hdr, msg.size);
cleanup:
	free_message(&msg, process);
	if (ret)
		dev_dbg(dxgglobaldev, "err: %s %d", __func__, ret);
	return ret;
}

struct d3dkmthandle
dxgvmb_send_create_context(struct dxgadapter *adapter,
			   struct dxgprocess *process,
			   struct d3dkmt_createcontextvirtual *args)
{
	struct dxgkvmb_command_createcontextvirtual *command = NULL;
	u32 cmd_size;
	int ret;
	struct d3dkmthandle context = {};
	struct dxgvmbusmsg msg = {.hdr = NULL};

	if (args->priv_drv_data_size > DXG_MAX_VM_BUS_PACKET_SIZE) {
		pr_err("PrivateDriverDataSize is invalid");
		ret = -EINVAL;
		goto cleanup;
	}
	cmd_size = sizeof(struct dxgkvmb_command_createcontextvirtual) +
	    args->priv_drv_data_size - 1;

	ret = init_message(&msg, adapter, process, cmd_size);
	if (ret)
		goto cleanup;
	command = (void *)msg.msg;

	command_vgpu_to_host_init2(&command->hdr,
				   DXGK_VMBCOMMAND_CREATECONTEXTVIRTUAL,
				   process->host_handle);
	command->device = args->device;
	command->node_ordinal = args->node_ordinal;
	command->engine_affinity = args->engine_affinity;
	command->flags = args->flags;
	command->client_hint = args->client_hint;
	command->priv_drv_data_size = args->priv_drv_data_size;
	if (args->priv_drv_data_size) {
		ret = copy_from_user(command->priv_drv_data,
				     args->priv_drv_data,
				     args->priv_drv_data_size);
		if (ret) {
			pr_err("%s Faled to copy private data",
				__func__);
			ret = -EINVAL;
			goto cleanup;
		}
	}
	/* Input command is returned back as output */
	ret = dxgvmb_send_sync_msg(msg.channel, msg.hdr, msg.size,
				   command, cmd_size);
	if (ret < 0) {
		goto cleanup;
	} else {
		context = command->context;
		if (args->priv_drv_data_size) {
			ret = copy_to_user(args->priv_drv_data,
					   command->priv_drv_data,
					   args->priv_drv_data_size);
			if (ret) {
				pr_err("%s Faled to copy private data to user",
					__func__);
				ret = -EINVAL;
				dxgvmb_send_destroy_context(adapter, process,
							    context);
				context.v = 0;
			}
		}
	}

cleanup:
	free_message(&msg, process);
	if (ret)
		dev_dbg(dxgglobaldev, "err: %s %d", __func__, ret);
	return context;
}

int dxgvmb_send_destroy_context(struct dxgadapter *adapter,
				struct dxgprocess *process,
				struct d3dkmthandle h)
{
	int ret;
	struct dxgkvmb_command_destroycontext *command;
	struct dxgvmbusmsg msg = {.hdr = NULL};

	ret = init_message(&msg, adapter, process, sizeof(*command));
	if (ret)
		goto cleanup;
	command = (void *)msg.msg;

	command_vgpu_to_host_init2(&command->hdr,
				   DXGK_VMBCOMMAND_DESTROYCONTEXT,
				   process->host_handle);
	command->context = h;

	ret = dxgvmb_send_sync_msg_ntstatus(msg.channel, msg.hdr, msg.size);
cleanup:
	free_message(&msg, process);
	if (ret)
		dev_dbg(dxgglobaldev, "err: %s %d", __func__, ret);
	return ret;
}

static int
copy_private_data(struct d3dkmt_createallocation *args,
		  struct dxgkvmb_command_createallocation *command,
		  struct d3dddi_allocationinfo2 *input_alloc_info,
		  struct d3dkmt_createstandardallocation *standard_alloc)
{
	struct dxgkvmb_command_createallocation_allocinfo *alloc_info;
	struct d3dddi_allocationinfo2 *input_alloc;
	int ret = 0;
	int i;
	u8 *private_data_dest = (u8 *) &command[1] +
	    (args->alloc_count *
	     sizeof(struct dxgkvmb_command_createallocation_allocinfo));

	if (args->private_runtime_data_size) {
		ret = copy_from_user(private_data_dest,
				     args->private_runtime_data,
				     args->private_runtime_data_size);
		if (ret) {
			pr_err("%s failed to copy runtime data", __func__);
			ret = -EINVAL;
			goto cleanup;
		}
		private_data_dest += args->private_runtime_data_size;
	}

	if (args->flags.standard_allocation) {
		dev_dbg(dxgglobaldev, "private data offset %d",
			(u32) (private_data_dest - (u8 *) command));

		args->priv_drv_data_size = sizeof(*args->standard_allocation);
		memcpy(private_data_dest, standard_alloc,
		       sizeof(*standard_alloc));
		private_data_dest += args->priv_drv_data_size;
	} else if (args->priv_drv_data_size) {
		ret = copy_from_user(private_data_dest,
				     args->priv_drv_data,
				     args->priv_drv_data_size);
		if (ret) {
			pr_err("%s failed to copy private data", __func__);
			ret = -EINVAL;
			goto cleanup;
		}
		private_data_dest += args->priv_drv_data_size;
	}

	alloc_info = (void *)&command[1];
	input_alloc = input_alloc_info;
	if (input_alloc_info[0].sysmem)
		command->flags.existing_sysmem = 1;
	for (i = 0; i < args->alloc_count; i++) {
		alloc_info->flags = input_alloc->flags.value;
		alloc_info->vidpn_source_id = input_alloc->vidpn_source_id;
		alloc_info->priv_drv_data_size =
		    input_alloc->priv_drv_data_size;
		if (input_alloc->priv_drv_data_size) {
			ret = copy_from_user(private_data_dest,
					     input_alloc->priv_drv_data,
					     input_alloc->priv_drv_data_size);
			if (ret) {
				pr_err("%s failed to copy alloc data",
					__func__);
				ret = -EINVAL;
				goto cleanup;
			}
			private_data_dest += input_alloc->priv_drv_data_size;
		}
		alloc_info++;
		input_alloc++;
	}

cleanup:
	if (ret)
		dev_dbg(dxgglobaldev, "err: %s %d", __func__, ret);
	return ret;
}

int create_existing_sysmem(struct dxgdevice *device,
			   struct dxgkvmb_command_allocinfo_return *host_alloc,
			   struct dxgallocation *dxgalloc,
			   bool read_only,
			   const void *sysmem)
{
	int ret1 = 0;
	void *kmem = NULL;
	int ret = 0;
	struct dxgkvmb_command_setexistingsysmemstore *set_store_command;
	u64 alloc_size = host_alloc->allocation_size;
	u32 npages = alloc_size >> PAGE_SHIFT;
	struct dxgvmbusmsg msg = {.hdr = NULL};

	ret = init_message(&msg, device->adapter, device->process,
			   sizeof(*set_store_command));
	if (ret)
		goto cleanup;
	set_store_command = (void *)msg.msg;

	/*
	 * Create a guest physical address list and set it as the allocation
	 * backing store in the host. This is done after creating the host
	 * allocation, because only now the allocation size is known.
	 */

	dev_dbg(dxgglobaldev, "   Alloc size: %lld", alloc_size);

	dxgalloc->cpu_address = (void *)sysmem;
	dxgalloc->pages = vzalloc(npages * sizeof(void *));
	if (dxgalloc->pages == NULL) {
		pr_err("failed to allocate pages");
		ret = -ENOMEM;
		goto cleanup;
	}
	ret1 = get_user_pages_fast((unsigned long)sysmem, npages, !read_only,
				  dxgalloc->pages);
	if (ret1 != npages) {
		pr_err("get_user_pages_fast failed: %d", ret1);
		if (ret1 > 0 && ret1 < npages)
			release_pages(dxgalloc->pages, ret1);
		vfree(dxgalloc->pages);
		dxgalloc->pages = NULL;
		ret = -ENOMEM;
		goto cleanup;
	}
	kmem = vmap(dxgalloc->pages, npages, VM_MAP, PAGE_KERNEL);
	if (kmem == NULL) {
		pr_err("vmap failed");
		ret = -ENOMEM;
		goto cleanup;
	}
	ret1 = vmbus_establish_gpadl(dxgglobal_get_vmbus(), kmem,
				     alloc_size, &dxgalloc->gpadl);
	if (ret1) {
		pr_err("establish_gpadl failed: %d", ret1);
		ret = -ENOMEM;
		goto cleanup;
	}
	dev_dbg(dxgglobaldev, "New gpadl %d", dxgalloc->gpadl.gpadl_handle);

	command_vgpu_to_host_init2(&set_store_command->hdr,
				   DXGK_VMBCOMMAND_SETEXISTINGSYSMEMSTORE,
				   device->process->host_handle);
	set_store_command->device = device->handle;
	set_store_command->device = device->handle;
	set_store_command->allocation = host_alloc->allocation;
	set_store_command->gpadl = dxgalloc->gpadl.gpadl_handle;
	ret = dxgvmb_send_sync_msg_ntstatus(msg.channel, msg.hdr, msg.size);
	if (ret < 0)
		pr_err("failed to set existing store: %x", ret);

cleanup:
	if (kmem)
		vunmap(kmem);
	free_message(&msg, device->process);
	if (ret)
		dev_dbg(dxgglobaldev, "err: %s %d", __func__, ret);
	return ret;
}

static int
process_allocation_handles(struct dxgprocess *process,
			   struct dxgdevice *device,
			   struct d3dkmt_createallocation *args,
			   struct dxgkvmb_command_createallocation_return *res,
			   struct dxgallocation **dxgalloc,
			   struct dxgresource *resource)
{
	int ret = 0;
	int i;

	hmgrtable_lock(&process->handle_table, DXGLOCK_EXCL);
	if (args->flags.create_resource) {
		ret = hmgrtable_assign_handle(&process->handle_table, resource,
					      HMGRENTRY_TYPE_DXGRESOURCE,
					      res->resource);
		if (ret < 0) {
			pr_err("failed to assign resource handle %x",
				   res->resource.v);
		} else {
			resource->handle = res->resource;
			resource->handle_valid = 1;
		}
	}
	for (i = 0; i < args->alloc_count; i++) {
		struct dxgkvmb_command_allocinfo_return *host_alloc;

		host_alloc = &res->allocation_info[i];
		ret = hmgrtable_assign_handle(&process->handle_table,
					      dxgalloc[i],
					      HMGRENTRY_TYPE_DXGALLOCATION,
					      host_alloc->allocation);
		if (ret < 0) {
			pr_err("failed to assign alloc handle %x %d %d",
				   host_alloc->allocation.v,
				   args->alloc_count, i);
			break;
		}
		dxgalloc[i]->alloc_handle = host_alloc->allocation;
		dxgalloc[i]->handle_valid = 1;
	}
	hmgrtable_unlock(&process->handle_table, DXGLOCK_EXCL);

	if (ret)
		dev_dbg(dxgglobaldev, "err: %s %d", __func__, ret);
	return ret;
}

static int
create_local_allocations(struct dxgprocess *process,
			 struct dxgdevice *device,
			 struct d3dkmt_createallocation *args,
			 struct d3dkmt_createallocation *__user input_args,
			 struct d3dddi_allocationinfo2 *alloc_info,
			 struct dxgkvmb_command_createallocation_return *result,
			 struct dxgresource *resource,
			 struct dxgallocation **dxgalloc,
			 u32 destroy_buffer_size)
{
	int i;
	int alloc_count = args->alloc_count;
	u8 *alloc_private_data = NULL;
	int ret = 0;
	int ret1;
	struct dxgkvmb_command_destroyallocation *destroy_buf;
	struct dxgvmbusmsg msg = {.hdr = NULL};

	ret = init_message(&msg, device->adapter, process,
			    destroy_buffer_size);
	if (ret)
		goto cleanup;
	destroy_buf = (void *)msg.msg;

	/* Prepare the command to destroy allocation in case of failure */
	command_vgpu_to_host_init2(&destroy_buf->hdr,
				   DXGK_VMBCOMMAND_DESTROYALLOCATION,
				   process->host_handle);
	destroy_buf->device = args->device;
	destroy_buf->resource = args->resource;
	destroy_buf->alloc_count = alloc_count;
	destroy_buf->flags.assume_not_in_use = 1;
	for (i = 0; i < alloc_count; i++) {
		dev_dbg(dxgglobaldev, "host allocation: %d %x",
			     i, result->allocation_info[i].allocation.v);
		destroy_buf->allocations[i] =
		    result->allocation_info[i].allocation;
	}

	if (args->flags.create_resource) {
		dev_dbg(dxgglobaldev, "new resource: %x", result->resource.v);
		ret = copy_to_user(&input_args->resource, &result->resource,
				   sizeof(struct d3dkmthandle));
		if (ret) {
			pr_err("%s failed to copy resource handle", __func__);
			ret = -EINVAL;
			goto cleanup;
		}
	}

	alloc_private_data = (u8 *) result +
	    sizeof(struct dxgkvmb_command_createallocation_return) +
	    sizeof(struct dxgkvmb_command_allocinfo_return) * (alloc_count - 1);

	for (i = 0; i < alloc_count; i++) {
		struct dxgkvmb_command_allocinfo_return *host_alloc;
		struct d3dddi_allocationinfo2 *user_alloc;

		host_alloc = &result->allocation_info[i];
		user_alloc = &alloc_info[i];
		dxgalloc[i]->num_pages =
		    host_alloc->allocation_size >> PAGE_SHIFT;
		if (user_alloc->sysmem) {
			ret = create_existing_sysmem(device, host_alloc,
						     dxgalloc[i],
						     args->flags.read_only != 0,
						     user_alloc->sysmem);
			if (ret < 0)
				goto cleanup;
		}
		dxgalloc[i]->cached = host_alloc->allocation_flags.cached;
		if (host_alloc->priv_drv_data_size) {
			ret = copy_to_user(user_alloc->priv_drv_data,
					   alloc_private_data,
					   host_alloc->priv_drv_data_size);
			if (ret) {
				pr_err("%s failed to copy private data",
					__func__);
				ret = -EINVAL;
				goto cleanup;
			}
			alloc_private_data += host_alloc->priv_drv_data_size;
		}
		ret = copy_to_user(&args->allocation_info[i].allocation,
				   &host_alloc->allocation,
				   sizeof(struct d3dkmthandle));
		if (ret) {
			pr_err("%s failed to copy alloc handle", __func__);
			ret = -EINVAL;
			goto cleanup;
		}
	}

	ret = process_allocation_handles(process, device, args, result,
					 dxgalloc, resource);
	if (ret < 0)
		goto cleanup;

	ret = copy_to_user(&input_args->global_share, &args->global_share,
			   sizeof(struct d3dkmthandle));
	if (ret) {
		pr_err("%s failed to copy global share", __func__);
		ret = -EINVAL;
	}

cleanup:

	if (ret < 0) {
		/* Free local handles before freeing the handles in the host */
		dxgdevice_acquire_alloc_list_lock(device);
		if (dxgalloc)
			for (i = 0; i < alloc_count; i++)
				if (dxgalloc[i])
					dxgallocation_free_handle(dxgalloc[i]);
		if (resource && args->flags.create_resource)
			dxgresource_free_handle(resource);
		dxgdevice_release_alloc_list_lock(device);

		/* Destroy allocations in the host to unmap gpadls */
		ret1 = dxgvmb_send_sync_msg_ntstatus(msg.channel, msg.hdr,
						     msg.size);
		if (ret1 < 0)
			pr_err("failed to destroy allocations: %x", ret1);

		dxgdevice_acquire_alloc_list_lock(device);
		if (dxgalloc) {
			for (i = 0; i < alloc_count; i++) {
				if (dxgalloc[i]) {
					dxgalloc[i]->alloc_handle.v = 0;
					dxgallocation_destroy(dxgalloc[i]);
					dxgalloc[i] = NULL;
				}
			}
		}
		if (resource && args->flags.create_resource) {
			/*
			 * Prevent the resource memory from freeing.
			 * It will be freed in the top level function.
			 */
			kref_get(&resource->resource_kref);
			dxgresource_destroy(resource);
		}
		dxgdevice_release_alloc_list_lock(device);
	}

	free_message(&msg, process);
	if (ret)
		dev_dbg(dxgglobaldev, "err: %s %d", __func__, ret);
	return ret;
}

int dxgvmb_send_create_allocation(struct dxgprocess *process,
				  struct dxgdevice *device,
				  struct d3dkmt_createallocation *args,
				  struct d3dkmt_createallocation *__user
				  input_args,
				  struct dxgresource *resource,
				  struct dxgallocation **dxgalloc,
				  struct d3dddi_allocationinfo2 *alloc_info,
				  struct d3dkmt_createstandardallocation
				  *standard_alloc)
{
	struct dxgkvmb_command_createallocation *command = NULL;
	struct dxgkvmb_command_createallocation_return *result = NULL;
	int ret = -EINVAL;
	int i;
	u32 result_size = 0;
	u32 cmd_size = 0;
	u32 destroy_buffer_size = 0;
	u32 priv_drv_data_size;
	struct dxgvmbusmsg msg = {.hdr = NULL};

	if (args->private_runtime_data_size >= DXG_MAX_VM_BUS_PACKET_SIZE ||
	    args->priv_drv_data_size >= DXG_MAX_VM_BUS_PACKET_SIZE) {
		ret = -EOVERFLOW;
		goto cleanup;
	}

	/*
	 * Preallocate the buffer, which will be used for destruction in case
	 * of a failure
	 */
	destroy_buffer_size = sizeof(struct dxgkvmb_command_destroyallocation) +
	    args->alloc_count * sizeof(struct d3dkmthandle);

	/* Compute the total private driver size */

	priv_drv_data_size = 0;

	for (i = 0; i < args->alloc_count; i++) {
		if (alloc_info[i].priv_drv_data_size >=
		    DXG_MAX_VM_BUS_PACKET_SIZE) {
			ret = -EOVERFLOW;
			goto cleanup;
		} else {
			priv_drv_data_size += alloc_info[i].priv_drv_data_size;
		}
		if (priv_drv_data_size >= DXG_MAX_VM_BUS_PACKET_SIZE) {
			ret = -EOVERFLOW;
			goto cleanup;
		}
	}

	/*
	 * Private driver data for the result includes only per allocation
	 * private data
	 */
	result_size = sizeof(struct dxgkvmb_command_createallocation_return) +
	    (args->alloc_count - 1) *
	    sizeof(struct dxgkvmb_command_allocinfo_return) +
	    priv_drv_data_size;
	result = vzalloc(result_size);
	if (result == NULL) {
		ret = -ENOMEM;
		goto cleanup;
	}

	/* Private drv data for the command includes the global private data */
	priv_drv_data_size += args->priv_drv_data_size;

	cmd_size = sizeof(struct dxgkvmb_command_createallocation) +
	    args->alloc_count *
	    sizeof(struct dxgkvmb_command_createallocation_allocinfo) +
	    args->private_runtime_data_size + priv_drv_data_size;
	if (cmd_size > DXG_MAX_VM_BUS_PACKET_SIZE) {
		ret = -EOVERFLOW;
		goto cleanup;
	}

	dev_dbg(dxgglobaldev, "command size, driver_data_size %d %d %ld %ld",
		    cmd_size, priv_drv_data_size,
		    sizeof(struct dxgkvmb_command_createallocation),
		    sizeof(struct dxgkvmb_command_createallocation_allocinfo));

	ret = init_message(&msg, device->adapter, process,
			   cmd_size);
	if (ret)
		goto cleanup;
	command = (void *)msg.msg;

	command_vgpu_to_host_init2(&command->hdr,
				   DXGK_VMBCOMMAND_CREATEALLOCATION,
				   process->host_handle);
	command->device = args->device;
	command->flags = args->flags;
	command->resource = args->resource;
	command->private_runtime_resource_handle =
	    args->private_runtime_resource_handle;
	command->alloc_count = args->alloc_count;
	command->private_runtime_data_size = args->private_runtime_data_size;
	command->priv_drv_data_size = args->priv_drv_data_size;

	ret = copy_private_data(args, command, alloc_info, standard_alloc);
	if (ret < 0)
		goto cleanup;

	ret = dxgvmb_send_sync_msg(msg.channel, msg.hdr, msg.size,
				   result, result_size);
	if (ret < 0) {
		pr_err("send_create_allocation failed %x", ret);
		goto cleanup;
	}

	ret = create_local_allocations(process, device, args, input_args,
				       alloc_info, result, resource, dxgalloc,
				       destroy_buffer_size);
cleanup:

	if (result)
		vfree(result);
	free_message(&msg, process);

	if (ret)
		dev_dbg(dxgglobaldev, "err: %s %d", __func__, ret);
	return ret;
}

int dxgvmb_send_destroy_allocation(struct dxgprocess *process,
				   struct dxgdevice *device,
				   struct d3dkmt_destroyallocation2 *args,
				   struct d3dkmthandle *alloc_handles)
{
	struct dxgkvmb_command_destroyallocation *destroy_buffer;
	u32 destroy_buffer_size;
	int ret;
	int allocations_size = args->alloc_count * sizeof(struct d3dkmthandle);
	struct dxgvmbusmsg msg = {.hdr = NULL};

	destroy_buffer_size = sizeof(struct dxgkvmb_command_destroyallocation) +
	    allocations_size;

	ret = init_message(&msg, device->adapter, process,
			    destroy_buffer_size);
	if (ret)
		goto cleanup;
	destroy_buffer = (void *)msg.msg;

	command_vgpu_to_host_init2(&destroy_buffer->hdr,
				   DXGK_VMBCOMMAND_DESTROYALLOCATION,
				   process->host_handle);
	destroy_buffer->device = args->device;
	destroy_buffer->resource = args->resource;
	destroy_buffer->alloc_count = args->alloc_count;
	destroy_buffer->flags = args->flags;
	if (allocations_size)
		memcpy(destroy_buffer->allocations, alloc_handles,
		       allocations_size);

	ret = dxgvmb_send_sync_msg_ntstatus(msg.channel, msg.hdr, msg.size);

cleanup:

	free_message(&msg, process);
	if (ret)
		dev_dbg(dxgglobaldev, "err: %s %d", __func__, ret);
	return ret;
}

int dxgvmb_send_get_stdalloc_data(struct dxgdevice *device,
				  enum d3dkmdt_standardallocationtype alloctype,
				  struct d3dkmdt_gdisurfacedata *alloc_data,
				  u32 physical_adapter_index,
				  u32 *alloc_priv_driver_size,
				  void *priv_alloc_data,
				  u32 *res_priv_data_size,
				  void *priv_res_data)
{
	struct dxgkvmb_command_getstandardallocprivdata *command;
	struct dxgkvmb_command_getstandardallocprivdata_return *result = NULL;
	u32 result_size = sizeof(*result);
	int ret;
	struct dxgvmbusmsgres msg = {.hdr = NULL};

	if (priv_alloc_data)
		result_size += *alloc_priv_driver_size;
	if (priv_res_data)
		result_size += *res_priv_data_size;
	ret = init_message_res(&msg, device->adapter, device->process,
			       sizeof(*command), result_size);
	if (ret)
		goto cleanup;
	command = msg.msg;
	result = msg.res;

	command_vgpu_to_host_init2(&command->hdr,
			DXGK_VMBCOMMAND_DDIGETSTANDARDALLOCATIONDRIVERDATA,
			device->process->host_handle);

	command->alloc_type = alloctype;
	command->priv_driver_data_size = *alloc_priv_driver_size;
	command->physical_adapter_index = physical_adapter_index;
	command->priv_driver_resource_size = *res_priv_data_size;
	switch (alloctype) {
	case _D3DKMDT_STANDARDALLOCATION_GDISURFACE:
		command->gdi_surface = *alloc_data;
		break;
	case _D3DKMDT_STANDARDALLOCATION_SHAREDPRIMARYSURFACE:
	case _D3DKMDT_STANDARDALLOCATION_SHADOWSURFACE:
	case _D3DKMDT_STANDARDALLOCATION_STAGINGSURFACE:
	default:
		pr_err("Invalid standard alloc type");
		goto cleanup;
	}

	ret = dxgvmb_send_sync_msg(msg.channel, msg.hdr, msg.size,
				   result, msg.res_size);
	if (ret < 0)
		goto cleanup;

	ret = ntstatus2int(result->status);
	if (ret < 0)
		goto cleanup;

	if (*alloc_priv_driver_size &&
	    result->priv_driver_data_size != *alloc_priv_driver_size) {
		pr_err("Priv data size mismatch");
		goto cleanup;
	}
	if (*res_priv_data_size &&
	    result->priv_driver_resource_size != *res_priv_data_size) {
		pr_err("Resource priv data size mismatch");
		goto cleanup;
	}
	*alloc_priv_driver_size = result->priv_driver_data_size;
	*res_priv_data_size = result->priv_driver_resource_size;
	if (priv_alloc_data) {
		memcpy(priv_alloc_data, &result[1],
		       result->priv_driver_data_size);
	}
	if (priv_res_data) {
		memcpy(priv_res_data,
		       (char *)(&result[1]) + result->priv_driver_data_size,
		       result->priv_driver_resource_size);
	}

cleanup:

	free_message((struct dxgvmbusmsg *)&msg, device->process);
	if (ret)
		dev_dbg(dxgglobaldev, "err: %s %d", __func__, ret);
	return ret;
}

int dxgvmb_send_query_adapter_info(struct dxgprocess *process,
				   struct dxgadapter *adapter,
				   struct d3dkmt_queryadapterinfo *args)
{
	struct dxgkvmb_command_queryadapterinfo *command;
	u32 cmd_size = sizeof(*command) + args->private_data_size - 1;
	int ret;
	u32 private_data_size;
	void *private_data;
	struct dxgvmbusmsg msg = {.hdr = NULL};

	ret = init_message(&msg, adapter, process, cmd_size);
	if (ret)
		goto cleanup;
	command = (void *)msg.msg;

	ret = copy_from_user(command->private_data,
			     args->private_data, args->private_data_size);
	if (ret) {
		pr_err("%s Faled to copy private data", __func__);
		ret = -EINVAL;
		goto cleanup;
	}

	command_vgpu_to_host_init2(&command->hdr,
				   DXGK_VMBCOMMAND_QUERYADAPTERINFO,
				   process->host_handle);
	command->private_data_size = args->private_data_size;
	command->query_type = args->type;

	if (dxgglobal->vmbus_ver >= DXGK_VMBUS_INTERFACE_VERSION) {
		private_data = msg.msg;
		private_data_size = command->private_data_size +
				    sizeof(struct ntstatus);
	} else {
		private_data = command->private_data;
		private_data_size = command->private_data_size;
	}

	ret = dxgvmb_send_sync_msg(msg.channel, msg.hdr, msg.size,
				   private_data, private_data_size);
	if (ret < 0)
		goto cleanup;

	if (dxgglobal->vmbus_ver >= DXGK_VMBUS_INTERFACE_VERSION) {
		ret = ntstatus2int(*(struct ntstatus *)private_data);
		if (ret < 0)
			goto cleanup;
		private_data = (char *)private_data + sizeof(struct ntstatus);
	}

	switch (args->type) {
	case _KMTQAITYPE_ADAPTERTYPE:
	case _KMTQAITYPE_ADAPTERTYPE_RENDER:
		{
			struct d3dkmt_adaptertype *adapter_type =
			    (void *)private_data;
			adapter_type->paravirtualized = 1;
			adapter_type->display_supported = 0;
			adapter_type->post_device = 0;
			adapter_type->indirect_display_device = 0;
			adapter_type->acg_supported = 0;
			adapter_type->support_set_timings_from_vidpn = 0;
			break;
		}
	default:
		break;
	}
	ret = copy_to_user(args->private_data, private_data,
			   args->private_data_size);
	if (ret) {
		pr_err("%s Faled to copy private data to user", __func__);
		ret = -EINVAL;
	}

cleanup:
	free_message(&msg, process);
	if (ret)
		dev_dbg(dxgglobaldev, "err: %s %d", __func__, ret);
	return ret;
}
