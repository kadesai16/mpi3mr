// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Driver for Broadcom MPI3 Storage Controllers
 *
 * Copyright (C) 2017-2021 Broadcom Inc.
 *  (mailto: mpi3mr-linuxdrv.pdl@broadcom.com)
 *
 */

#include "mpi3mr.h"
#include "mpi3mr_app.h"

static struct fasync_struct *mpi3mr_app_async_queue;

/**
 * mpi3mr_verify_adapter - verify adapter number is valid
 * @ioc_number: Adapter number
 * @mriocpp: Pointer to hold per adapter instance
 *
 * This function checks whether given adapter number matches
 * with an adapter id in the driver's list and if so fills
 * pointer to the per adapter instance in mriocpp else set that
 * to NULL.
 *
 * Return: Nothing.
 */
static void mpi3mr_verify_adapter(int ioc_number, struct mpi3mr_ioc **mriocpp)
{
	struct mpi3mr_ioc *mrioc;

	spin_lock(&mrioc_list_lock);
	list_for_each_entry(mrioc, &mrioc_list, list) {
		if (mrioc->id != ioc_number)
			continue;
		spin_unlock(&mrioc_list_lock);
		*mriocpp = mrioc;
		return;
	}
	spin_unlock(&mrioc_list_lock);
	*mriocpp = NULL;
}

/**
 * mpi3mr_get_all_tgt_info - Get all target information
 * @mrioc: Adapter instance reference
 * @data_in_buf: User buffer to copy the target information
 * @data_in_sz: length of the user buffer.
 *
 * This function copies the driver managed target devices device
 * handle, persistent ID, bus ID and taret ID to the user
 * provided buffer for the specific controller. This function
 * also provides the number of devices managed by the driver for
 * the specific controller.
 *
 * Return: 0 on success and proper error codes on failure
 */
static long mpi3mr_get_all_tgt_info(struct mpi3mr_ioc *mrioc,
		void __user *data_in_buf, uint32_t data_in_sz)
{
	long rval = 0, devmap_info_sz;
	u16 num_devices = 0, i = 0;
	unsigned long flags;
	struct mpi3mr_tgt_dev *tgtdev;
	struct mpi3mr_device_map_info *devmap_info = NULL;
	struct mpi3mr_all_tgt_info __user *all_tgt_info =
		(struct mpi3mr_all_tgt_info *)data_in_buf;
	u32 min_entrylen, kern_entrylen = 0, usr_entrylen;

	if (data_in_sz < sizeof(u32)) {
		dbgprint(mrioc, "failure at %s:%d/%s()!\n",
			 __FILE__, __LINE__, __func__);
		return -EINVAL;
	}

	devmap_info_sz = sizeof(struct mpi3mr_device_map_info);

	spin_lock_irqsave(&mrioc->tgtdev_lock, flags);
	list_for_each_entry(tgtdev, &mrioc->tgtdev_list, list)
		num_devices++;
	spin_unlock_irqrestore(&mrioc->tgtdev_lock, flags);

	usr_entrylen = (data_in_sz - sizeof(u32)) / devmap_info_sz;
	usr_entrylen *= devmap_info_sz;

	if (!num_devices || !usr_entrylen)
		goto copy_usrbuf;

	devmap_info = kcalloc(num_devices, devmap_info_sz, GFP_KERNEL);
	if (!devmap_info)
		return -ENOMEM;

	kern_entrylen = num_devices * devmap_info_sz;
	memset((u8 *)devmap_info, 0xFF, kern_entrylen);
	spin_lock_irqsave(&mrioc->tgtdev_lock, flags);
	list_for_each_entry(tgtdev, &mrioc->tgtdev_list, list) {
		if (i >= num_devices)
			break;
		devmap_info[i].handle = tgtdev->dev_handle;
		devmap_info[i].perst_id = tgtdev->perst_id;
		if (tgtdev->host_exposed && tgtdev->starget) {
			devmap_info[i].target_id = tgtdev->starget->id;
			devmap_info[i].bus_id = tgtdev->starget->channel;
		}
		i++;
	}
	num_devices = i;
	spin_unlock_irqrestore(&mrioc->tgtdev_lock, flags);

copy_usrbuf:
	if (copy_to_user(&all_tgt_info->num_devices, &num_devices,
			 sizeof(num_devices)))
		rval = -EFAULT;
	else {
		min_entrylen = min(usr_entrylen, kern_entrylen);
		if (min_entrylen &&
			copy_to_user(&all_tgt_info->dmi,
				      devmap_info, min_entrylen))
			rval = -EFAULT;
	}

	kfree(devmap_info);
	return rval;
}

/**
 * mpi3mr_enable_logdata - Handler for log data enable IOCTL
 * @mrioc: Adapter instance reference
 * @data_in_buf: User buffer to copy the max logdata entry count
 * @data_in_sz: length of the user buffer.
 *
 * This function enables log data caching in the driver if not
 * already enabled and return the maximum number of log data
 * entries that can be cached in the driver.
 *
 * Return: 0 on success and proper error codes on failure
 */
static long mpi3mr_enable_logdata(struct mpi3mr_ioc *mrioc,
		void __user *data_in_buf, uint32_t data_in_sz)
{
	struct mpi3mr_logdata_enable logdata_enable;
	u16 entry_size;

	entry_size = mrioc->facts.reply_sz -
			(sizeof(struct mpi3_event_notification_reply) - 4);
	entry_size += MPI3MR_IOCTL_LOGDATA_ENTRY_HEADER_SZ;
	logdata_enable.max_entries = MPI3MR_IOCTL_LOGDATA_MAX_ENTRIES;

	if (!mrioc->logdata_buf) {
		mrioc->logdata_buf_idx = 0;
		mrioc->logdata_entry_sz = entry_size;
		mrioc->logdata_buf =
			kcalloc(MPI3MR_IOCTL_LOGDATA_MAX_ENTRIES,
					entry_size, GFP_KERNEL);
		if (!mrioc->logdata_buf)
			return -ENOMEM;
	}

	if (copy_to_user(data_in_buf, &logdata_enable, sizeof(logdata_enable)))
		return -EFAULT;

	return 0;
}

/**
 * mpi3mr_get_logdata - Handler for get log data  IOCTL
 * @mrioc: Adapter instance reference
 * @data_in_buf: User buffer to copy the logdata entries
 * @data_in_sz: length of the user buffer.
 *
 * This function copies the log data entries to the user buffer
 * when log caching is enabled in the driver.
 *
 * Return: 0 on success and proper error codes on failure
 */
static long mpi3mr_get_logdata(struct mpi3mr_ioc *mrioc,
		void __user *data_in_buf, uint32_t data_in_sz)
{
	u16 num_entries, sz, entry_sz;

	entry_sz = mrioc->logdata_entry_sz;
	if ((!mrioc->logdata_buf) || (data_in_sz < entry_sz))
		return -EINVAL;

	num_entries = data_in_sz / entry_sz;
	num_entries = min_t(int, num_entries,
				MPI3MR_IOCTL_LOGDATA_MAX_ENTRIES);
	sz = num_entries * entry_sz;

	if (copy_to_user(data_in_buf, mrioc->logdata_buf, sz))
		return -EFAULT;

	return 0;
}

/**
 * mpi3mr_get_change_count - Get topology change count
 * @mrioc: Adapter instance reference
 * @data_in_buf: User buffer to copy the change count
 * @data_in_sz: length of the user buffer.
 *
 * This function copies the topology change count provided by the
 * driver in events and cached in the driver to the user
 * provided buffer for the specific controller.
 *
 * Return: 0 on success and proper error codes on failure
 */
static long mpi3mr_get_change_count(struct mpi3mr_ioc *mrioc,
		void __user *data_in_buf, uint32_t data_in_sz)
{
	struct mpi3mr_change_count chgcnt;

	chgcnt.change_count = mrioc->change_count;
	if (copy_to_user(data_in_buf, &chgcnt, sizeof(chgcnt)))
		return -EFAULT;

	return 0;
}

/**
 * mpi3mr_ioctl_adp_reset - Issue controller reset
 * @mrioc: Adapter instance reference
 * @data_out_buf: User buffer with reset type
 * @data_out_sz: length of the user buffer.
 *
 * This function identifies the user provided reset type and
 * issues approporiate reset to the controller and.wait for that
 * to complete and reinitialize the controller and then returns
 *
 * Return: 0 on success and proper error codes on failure
 */
static long mpi3mr_ioctl_adp_reset(struct mpi3mr_ioc *mrioc,
		void __user *data_out_buf, uint32_t data_out_sz)
{
	long rval = 0;
	struct mpi3mr_ioctl_adp_reset adpreset;
	u8 save_snapdump;

	if (copy_from_user(&adpreset, data_out_buf, sizeof(adpreset)))
		return -EFAULT;

	switch (adpreset.reset_type) {
	case MPI3MR_IOCTL_ADPRESET_SOFT:
		save_snapdump = 0;
		break;
	case MPI3MR_IOCTL_ADPRESET_DIAG_FAULT:
		save_snapdump = 1;
		break;
	default:
		dbgprint(mrioc, "%s: unknown reset_type(%d)\n",
		    __func__, adpreset.reset_type);
		return -EINVAL;
	}

	rval = mpi3mr_soft_reset_handler(mrioc, MPI3MR_RESET_FROM_IOCTL,
	    save_snapdump);

	if (rval)
		dbgprint(mrioc,
		    "%s: reset handler returned error(%ld) for reset type %d\n",
		    __func__, rval, adpreset.reset_type);

	return rval;
}

/**
 * mpi3mr_populate_adpinfo - Get adapter info IOCTL handler
 * @mrioc: Adapter instance reference
 * @data_in_buf: User buffer to hold adapter information
 * @data_in_sz: length of the user buffer.
 *
 * This function provides adapter information for the given
 * controller
 *
 * Return: 0 on success and proper error codes on failure
 */
static long mpi3mr_populate_adpinfo(struct mpi3mr_ioc *mrioc,
		void __user *data_in_buf, uint32_t data_in_sz)
{
	struct mpi3mr_adp_info adpinfo;

	memset(&adpinfo, 0, sizeof(adpinfo));
	adpinfo.adp_type = MPI3MR_IOCTL_ADPTYPE_AVGFAMILY;
	adpinfo.pci_dev_id = mrioc->pdev->device;
	adpinfo.pci_dev_hw_rev = mrioc->pdev->revision;
	adpinfo.pci_subsys_dev_id = mrioc->pdev->subsystem_device;
	adpinfo.pci_subsys_ven_id = mrioc->pdev->subsystem_vendor;
	adpinfo.pci_bus = mrioc->pdev->bus->number;
	adpinfo.pci_dev = PCI_SLOT(mrioc->pdev->devfn);
	adpinfo.pci_func = PCI_FUNC(mrioc->pdev->devfn);
	adpinfo.pci_seg_id = pci_domain_nr(mrioc->pdev->bus);
	adpinfo.ioctl_ver = MPI3MR_IOCTL_VERSION;
	memcpy((u8 *)&adpinfo.driver_info, (u8 *)&mrioc->driver_info,
	       sizeof(adpinfo.driver_info));

	if (copy_to_user(data_in_buf, &adpinfo, sizeof(adpinfo)))
		return -EFAULT;

	return 0;
}

/**
 * mpi3mr_ioctl_process_drv_cmds - Driver IOCTL handler
 * @mrioc: Adapter instance reference
 * @arg: User data payload buffer for the IOCTL
 *
 * This function is the top level handler for driver commands,
 * this does basic validation of the buffer and identifies the
 * opcode and switches to correct sub handler.
 *
 * Return: 0 on success and proper error codes on failure
 */
static long
mpi3mr_ioctl_process_drv_cmds(struct file *file, void __user *arg)
{
	long rval = 0;
	struct mpi3mr_ioc *mrioc = NULL;
	struct mpi3mr_ioctl_drv_cmd karg;

	if (copy_from_user(&karg, arg, sizeof(karg)))
		return -EFAULT;

	mpi3mr_verify_adapter(karg.mrioc_id, &mrioc);
	if (!mrioc)
		return -ENODEV;

	if (file->f_flags & O_NONBLOCK) {
		if (!mutex_trylock(&mrioc->ioctl_cmds.mutex))
			return -EAGAIN;
	} else if (mutex_lock_interruptible(&mrioc->ioctl_cmds.mutex))
		return -ERESTARTSYS;

	switch (karg.opcode) {
	case MPI3MR_DRVIOCTL_OPCODE_ADPINFO:
		rval = mpi3mr_populate_adpinfo(mrioc, karg.data_in_buf,
					karg.data_in_size);
		break;
	case MPI3MR_DRVIOCTL_OPCODE_ADPRESET:
		rval = mpi3mr_ioctl_adp_reset(mrioc, karg.data_out_buf,
					karg.data_out_size);
		break;
	case MPI3MR_DRVIOCTL_OPCODE_ALLTGTDEVINFO:
		rval = mpi3mr_get_all_tgt_info(mrioc, karg.data_in_buf,
					karg.data_in_size);
		break;
	case MPI3MR_DRVIOCTL_OPCODE_LOGDATAENABLE:
		rval = mpi3mr_enable_logdata(mrioc, karg.data_in_buf,
					karg.data_in_size);
		break;
	case MPI3MR_DRVIOCTL_OPCODE_GETLOGDATA:
		rval = mpi3mr_get_logdata(mrioc, karg.data_in_buf,
					karg.data_in_size);
		break;
	case MPI3MR_DRVIOCTL_OPCODE_GETCHGCNT:
		rval = mpi3mr_get_change_count(mrioc, karg.data_in_buf,
					karg.data_in_size);
		break;
	case MPI3MR_DRVIOCTL_OPCODE_UNKNOWN:
	default:
		rval = -EINVAL;
		dbgprint(mrioc, "Unsupported drv ioctl opcode 0x%x\n",
			 karg.opcode);
		break;
	}
	mutex_unlock(&mrioc->ioctl_cmds.mutex);
	return rval;
}

/**
 * mpi3mr_ioctl_build_sgl - SGL consturction for MPI IOCTLs
 * @mpi_req: MPI request
 * @sgl_offset: offset to start SGL in the MPI request
 * @dma_buffers: DMA address of the buffers to be placed in SGL
 * @bufcnt: Number of DMA buffers
 * @is_rmc: Does the buffer list has management command buffer
 * @is_rmr: Does the buffer list has management response buffer
 * @num_data_sges: Number of data buffers in the list
 *
 * This function places the DMA address of the given buffers in
 * proper format as SGEs in the given MPI request.
 *
 * Return: Nothing
 */
static void mpi3mr_ioctl_build_sgl(u8 *mpi_req, uint32_t sgl_offset,
		struct mpi3mr_buf_map *dma_buffers,
		u8 bufcnt, bool is_rmc, bool is_rmr, u8 num_data_sges)
{
	u8 *sgl;
	u8 sgl_flags, sgl_flags_last, count = 0;
	struct mpi3_mgmt_passthrough_request *mgmt_pt_req;
	struct mpi3mr_buf_map *dma_buff;

	sgl = (mpi_req + sgl_offset);
	mgmt_pt_req = (struct mpi3_mgmt_passthrough_request *)mpi_req;
	dma_buff = dma_buffers;

	sgl_flags = MPI3_SGE_FLAGS_ELEMENT_TYPE_SIMPLE |
			MPI3_SGE_FLAGS_DLAS_SYSTEM |
			MPI3_SGE_FLAGS_END_OF_BUFFER;

	sgl_flags_last = sgl_flags | MPI3_SGE_FLAGS_END_OF_LIST;

	if (is_rmc) {
		mpi3mr_add_sg_single(&mgmt_pt_req->command_sgl,
				     sgl_flags_last, dma_buff->kern_buf_len,
				     dma_buff->kern_buf_dma);
		sgl = (u8 *)dma_buff->kern_buf + dma_buff->user_buf_len;
		dma_buff++;
		count++;
		if (is_rmr) {
			mpi3mr_add_sg_single(&mgmt_pt_req->response_sgl,
					sgl_flags_last,
					dma_buff->kern_buf_len,
					dma_buff->kern_buf_dma);
			dma_buff++;
			count++;
		} else
			mpi3mr_build_zero_len_sge(&mgmt_pt_req->response_sgl);
	}
	if (!num_data_sges) {
		mpi3mr_build_zero_len_sge(sgl);
		return;
	}
	for (; count < bufcnt; count++, dma_buff++) {
		if (dma_buff->data_dir == DMA_BIDIRECTIONAL)
			continue;
		if (num_data_sges == 1 || !is_rmc)
			mpi3mr_add_sg_single(sgl, sgl_flags_last,
					     dma_buff->kern_buf_len,
					     dma_buff->kern_buf_dma);
		else
			mpi3mr_add_sg_single(sgl, sgl_flags,
					     dma_buff->kern_buf_len,
					     dma_buff->kern_buf_dma);
		sgl += sizeof(struct mpi3_sge_common);
		num_data_sges--;
	}
}

/**
 * mpi3mr_ioctl_process_mpt_cmds - MPI Pass through IOCTL handler
 * @mrioc: Adapter instance reference
 * @arg: User data payload buffer for the IOCTL
 *
 * This function is the top level handler for MPI Pass through
 * IOCTL, this does basic validation of the input data buffers,
 * identifies the given buffer types and MPI command, allocates
 * DMAable memory for user given buffers, constructs SGL
 * properly and passes the command to the firmware.
 *
 * Once the MPI command is completed the driver copies the data
 * if any and reply, sense information to user provided buffers.
 * If the command is timed out then issues controller reset
 * prior to returning.
 *
 * Return: 0 on success and proper error codes on failure
 */
static long mpi3mr_ioctl_process_mpt_cmds(struct file *file,
	void __user *arg)
{
	long rval = -EINVAL;
	struct mpi3mr_ioc *mrioc = NULL;
	struct mpi3mr_ioctl_mptcmd karg;
	struct mpi3mr_ioctl_buf_entry_list *buffer_list = NULL;
	struct mpi3mr_buf_entry *buf_entries = NULL;
	struct mpi3mr_buf_map *dma_buffers = NULL, *dma_buff;
	struct mpi3_request_header *mpi_header = NULL;
	struct mpi3_status_reply_descriptor *status_desc;
	struct mpi3mr_ioctl_reply_buf *ioctl_reply_buf = NULL;
	u8 *mpi_req = NULL, *sense_buff_k = NULL;
	u8 count, bufcnt, din_cnt = 0, dout_cnt = 0;
	u8 erb_offset = 0xFF, reply_offset = 0xFF, sg_entries = 0;
	bool invalid_be = false, is_rmcb = false, is_rmrb = false;
	u32 tmplen;

	if (copy_from_user(&karg, arg, sizeof(karg)))
		return -EFAULT;

	mpi3mr_verify_adapter(karg.mrioc_id, &mrioc);
	if (!mrioc)
		return -ENODEV;

	if (karg.timeout < MPI3MR_IOCTL_DEFAULT_TIMEOUT)
		karg.timeout = MPI3MR_IOCTL_DEFAULT_TIMEOUT;

	if (!karg.buf_entry_list_size || !karg.mpi_msg_size)
		return -EINVAL;

	if ((karg.mpi_msg_size * 4) > MPI3MR_ADMIN_REQ_FRAME_SZ)
		return -EINVAL;

	mpi_req = kzalloc(MPI3MR_ADMIN_REQ_FRAME_SZ, GFP_KERNEL);
	if (!mpi_req)
		return -ENOMEM;

	mpi_header = (struct mpi3_request_header *)mpi_req;

	if (copy_from_user(mpi_req, karg.mpi_msg_buf,
			   (karg.mpi_msg_size * 4)))
		goto out;

	buffer_list = kzalloc(karg.buf_entry_list_size, GFP_KERNEL);
	if (!buffer_list) {
		rval = -ENOMEM;
		goto out;
	}

	if (copy_from_user(buffer_list, karg.buf_entry_list,
			   karg.buf_entry_list_size)) {
		rval = -EFAULT;
		goto out;
	}

	if (!buffer_list->num_of_entries) {
		rval = -EINVAL;
		goto out;
	}

	bufcnt = buffer_list->num_of_entries;
	dma_buffers = kzalloc((sizeof(struct mpi3mr_buf_map) * bufcnt), GFP_KERNEL);
	if (!dma_buffers) {
		rval = -ENOMEM;
		goto out;
	}

	buf_entries = buffer_list->buf_entry;
	dma_buff = dma_buffers;
	for (count = 0; count < bufcnt; count++, buf_entries++, dma_buff++) {
		dma_buff->user_buf = buf_entries->buffer;
		dma_buff->user_buf_len = buf_entries->buf_len;

		switch (buf_entries->buf_type) {
		case MPI3MR_IOCTL_BUFTYPE_RAIDMGMT_CMD:
			is_rmcb = true;
			if (count != 0)
				invalid_be = true;
			dma_buff->data_dir = DMA_FROM_DEVICE;
			break;
		case MPI3MR_IOCTL_BUFTYPE_RAIDMGMT_RESP:
			is_rmrb = true;
			if (count != 1 || !is_rmcb)
				invalid_be = true;
			dma_buff->data_dir = DMA_TO_DEVICE;
			break;
		case MPI3MR_IOCTL_BUFTYPE_DATA_IN:
			din_cnt++;
			if ((din_cnt > 1) && !is_rmcb)
				invalid_be = true;
			dma_buff->data_dir = DMA_TO_DEVICE;
			break;
		case MPI3MR_IOCTL_BUFTYPE_DATA_OUT:
			dout_cnt++;
			if ((dout_cnt > 1) && !is_rmcb)
				invalid_be = true;
			dma_buff->data_dir = DMA_FROM_DEVICE;
			break;
		case MPI3MR_IOCTL_BUFTYPE_MPI_REPLY:
			reply_offset = count;
			dma_buff->data_dir = DMA_BIDIRECTIONAL;
			break;
		case MPI3MR_IOCTL_BUFTYPE_ERR_RESPONSE:
			erb_offset = count;
			dma_buff->data_dir = DMA_BIDIRECTIONAL;
			break;
		default:
			invalid_be = true;
			break;
		}
		if (invalid_be)
			break;
	}
	if (invalid_be) {
		rval = -EINVAL;
		goto out;
	}

	if (!is_rmcb && (dout_cnt || din_cnt)) {
		sg_entries = dout_cnt + din_cnt;
		if (((karg.mpi_msg_size * 4) + (sg_entries *
		      sizeof(struct mpi3_sge_common))) > MPI3MR_ADMIN_REQ_FRAME_SZ) {
			rval = -EINVAL;
			goto out;
		}
	}

	dma_buff = dma_buffers;
	for (count = 0; count < bufcnt; count++, dma_buff++) {
		dma_buff->kern_buf_len = dma_buff->user_buf_len;
		if (is_rmcb && !count)
			dma_buff->kern_buf_len += ((dout_cnt + din_cnt) *
			    sizeof(struct mpi3_sge_common));
		if ((count == reply_offset) || (count == erb_offset)) {
			dma_buff->kern_buf_len = 0;
			continue;
		}
		if (!dma_buff->kern_buf_len)
			continue;

		dma_buff->kern_buf = dma_alloc_coherent(&mrioc->pdev->dev,
						dma_buff->kern_buf_len,
						&dma_buff->kern_buf_dma,
						GFP_KERNEL);
		if (!dma_buff->kern_buf) {
			rval = -ENOMEM;
			goto out;
		}
		if (dma_buff->data_dir == DMA_FROM_DEVICE) {
			tmplen = min(dma_buff->kern_buf_len,
					dma_buff->user_buf_len);
			if (copy_from_user(dma_buff->kern_buf,
					dma_buff->user_buf, tmplen)) {
				rval = -EFAULT;
				goto out;
			}
		}
	}
	if (erb_offset != 0xFF) {
		sense_buff_k = kzalloc(MPI3MR_SENSE_BUF_SZ, GFP_KERNEL);
		if (!sense_buff_k) {
			rval = -ENOMEM;
			goto out;
		}
	}

	if (mpi_header->function != MPI3_FUNCTION_NVME_ENCAPSULATED)
		mpi3mr_ioctl_build_sgl(mpi_req, (karg.mpi_msg_size * 4),
					dma_buffers, bufcnt, is_rmcb,
					is_rmrb, (dout_cnt + din_cnt));

	if (file->f_flags & O_NONBLOCK) {
		if (!mutex_trylock(&mrioc->ioctl_cmds.mutex)) {
			rval = -EAGAIN;
			goto out;
		}
	} else if (mutex_lock_interruptible(&mrioc->ioctl_cmds.mutex)) {
		rval = -ERESTARTSYS;
		goto out;
	}
	if (mrioc->ioctl_cmds.state & MPI3MR_CMD_PENDING) {
		rval = -EAGAIN;
		dbgprint(mrioc, "%s command is in use\n", __func__);
		mutex_unlock(&mrioc->ioctl_cmds.mutex);
		goto out;
	}
	if (mrioc->reset_in_progress) {
		dbgprint(mrioc, "%s reset in progress\n", __func__);
		rval = -EAGAIN;
		mutex_unlock(&mrioc->ioctl_cmds.mutex);
		goto out;
	}
	if (mrioc->block_ioctls) {
		dbgprint(mrioc, "%s IOCTLs are blocked\n", __func__);
		rval = -EAGAIN;
		mutex_unlock(&mrioc->ioctl_cmds.mutex);
		goto out;
	}

	mrioc->ioctl_cmds.state = MPI3MR_CMD_PENDING;
	mrioc->ioctl_cmds.is_waiting = 1;
	mrioc->ioctl_cmds.callback = NULL;
	mrioc->ioctl_cmds.is_sense = false;
	mrioc->ioctl_cmds.sensebuf = sense_buff_k;
	memset(mrioc->ioctl_cmds.reply, 0, mrioc->facts.reply_sz);
	mpi_header->host_tag = cpu_to_le16(MPI3MR_HOSTTAG_IOCTLCMDS);
	init_completion(&mrioc->ioctl_cmds.done);
	rval = mpi3mr_admin_request_post(mrioc, mpi_req,
					 MPI3MR_ADMIN_REQ_FRAME_SZ, 0);
	if (rval) {
		rval = -EAGAIN;
		goto out_unlock;
	}
	wait_for_completion_timeout(&mrioc->ioctl_cmds.done,
				    (karg.timeout * HZ));
	if (!(mrioc->ioctl_cmds.state & MPI3MR_CMD_COMPLETE)) {
		mrioc->ioctl_cmds.is_waiting = 0;
		dbgprint(mrioc, "%s command timed out\n", __func__);
		rval = -EFAULT;
		mpi3mr_soft_reset_handler(mrioc,
				MPI3MR_RESET_FROM_IOCTL_TIMEOUT, 1);
		goto out_unlock;
	}

	if ((mrioc->ioctl_cmds.ioc_status & MPI3_IOCSTATUS_STATUS_MASK)
	     != MPI3_IOCSTATUS_SUCCESS) {
		dbgprint(mrioc,
			"%s ioc_status(0x%04x)  Loginfo(0x%08x)\n", __func__,
			(mrioc->ioctl_cmds.ioc_status & MPI3_IOCSTATUS_STATUS_MASK),
			mrioc->ioctl_cmds.ioc_loginfo);
	}

	if ((reply_offset != 0xFF) && dma_buffers[reply_offset].user_buf_len) {
		dma_buff = &dma_buffers[reply_offset];
		dma_buff->kern_buf_len =
			(sizeof(struct mpi3mr_ioctl_reply_buf) - 1
			+ mrioc->facts.reply_sz);
		ioctl_reply_buf = kzalloc(dma_buff->kern_buf_len, GFP_KERNEL);

		if (!ioctl_reply_buf) {
			rval = -ENOMEM;
			goto out_unlock;
		}
		if (mrioc->ioctl_cmds.state & MPI3MR_CMD_REPLY_VALID) {
			ioctl_reply_buf->mpi_reply_type =
				MPI3MR_IOCTL_MPI_REPLY_BUFTYPE_ADDRESS;
			memcpy(ioctl_reply_buf->ioctl_reply_buf,
					mrioc->ioctl_cmds.reply,
					mrioc->facts.reply_sz);
		} else {
			ioctl_reply_buf->mpi_reply_type =
				MPI3MR_IOCTL_MPI_REPLY_BUFTYPE_STATUS;
			status_desc = (struct mpi3_status_reply_descriptor *)
				ioctl_reply_buf->ioctl_reply_buf;
			status_desc->ioc_status = mrioc->ioctl_cmds.ioc_status;
			status_desc->ioc_log_info = mrioc->ioctl_cmds.ioc_loginfo;
		}
		tmplen = min(dma_buff->kern_buf_len, dma_buff->user_buf_len);
		if (copy_to_user(dma_buff->user_buf, ioctl_reply_buf, tmplen)) {
			rval = -EFAULT;
			goto out_unlock;
		}
	}

	if (erb_offset != 0xFF && mrioc->ioctl_cmds.sensebuf &&
	    mrioc->ioctl_cmds.is_sense) {
		dma_buff = &dma_buffers[erb_offset];
		tmplen = min_t(int, dma_buff->user_buf_len,
				MPI3MR_SENSE_BUF_SZ);
		if (copy_to_user(dma_buff->user_buf, sense_buff_k, tmplen)) {
			rval = -EFAULT;
			goto out_unlock;
		}
	}

	dma_buff = dma_buffers;
	for (count = 0; count < bufcnt; count++, dma_buff++) {
		if (dma_buff->data_dir == DMA_TO_DEVICE) {
			tmplen = min(dma_buff->kern_buf_len,
					dma_buff->user_buf_len);
			if (copy_to_user(dma_buff->user_buf,
					dma_buff->kern_buf, tmplen))
				rval = -EFAULT;
		}
	}

out_unlock:
	mrioc->ioctl_cmds.is_sense = false;
	mrioc->ioctl_cmds.sensebuf = NULL;
	mrioc->ioctl_cmds.state = MPI3MR_CMD_NOTUSED;
	mutex_unlock(&mrioc->ioctl_cmds.mutex);
out:
	kfree(sense_buff_k);
	kfree(buffer_list);
	kfree(mpi_req);
	if (dma_buffers) {
		dma_buff = dma_buffers;
		for (count = 0; count < bufcnt; count++, dma_buff++) {
			if (dma_buff->kern_buf && dma_buff->kern_buf_dma)
				dma_free_coherent(&mrioc->pdev->dev,
						dma_buff->kern_buf_len,
						dma_buff->kern_buf,
						dma_buff->kern_buf_dma);
		}
		kfree(dma_buffers);
	}
	kfree(ioctl_reply_buf);
	return rval;
}

/**
 * mpi3mr_ioctl - Main IOCTL handler
 * @file: File pointer
 * @cmd: IOCTL command
 * @arg: User data payload buffer for the IOCTL
 *
 * This is main IOCTL handler which checks the command type and
 * executes proper sub handler specific for the command.
 *
 * Return: 0 on success and proper error codes on failure
 */
static long mpi3mr_ioctl(struct file *file, unsigned int cmd,
			     unsigned long arg)
{
	long rval = -EINVAL;

	switch (cmd) {
	case MPI3MRDRVCMD:
		if (_IOC_SIZE(cmd) == sizeof(struct mpi3mr_ioctl_drv_cmd))
			rval = mpi3mr_ioctl_process_drv_cmds(file,
					(void __user *)arg);
		break;
	case MPI3MRMPTCMD:
		if (_IOC_SIZE(cmd) == sizeof(struct mpi3mr_ioctl_mptcmd))
			rval = mpi3mr_ioctl_process_mpt_cmds(file,
					(void __user *)arg);
		break;
	default:
		pr_err("%s:Unsupported ioctl cmd (0x%08x)\n", __func__, cmd);
		break;
	}
	return rval;
}

/**
 * mpi3mr_app_fasync - fasync callback
 * @fd: File descriptor
 * @filep: File pointer
 * @mode: Mode
 *
 * Return: fasync_helper() returned value
 */
static int mpi3mr_app_fasync(int fd, struct file *filep, int mode)
{
	return fasync_helper(fd, filep, mode, &mpi3mr_app_async_queue);
}

static const struct file_operations mpi3mr_app_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = mpi3mr_ioctl,
	.fasync = mpi3mr_app_fasync,
};

static struct miscdevice mpi3mr_app_dev = {
	.minor  = MPI3MR_MINOR,
	.name   = MPI3MR_DEV_NAME,
	.fops   = &mpi3mr_app_fops,
};

/**
 * mpi3mr_app_init - Character driver interface initializer
 *
 */
void mpi3mr_app_init(void)
{
	mpi3mr_app_async_queue = NULL;

	if (misc_register(&mpi3mr_app_dev) < 0)
		pr_err("%s can't register misc device [minor=%d]\n",
		       MPI3MR_DRIVER_NAME, MPI3MR_MINOR);
}

/**
 * mpi3mr_app_exit - Character driver interface cleanup function
 *
 */
void mpi3mr_app_exit(void)
{
	misc_deregister(&mpi3mr_app_dev);
}
