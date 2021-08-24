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
static DECLARE_WAIT_QUEUE_HEAD(_app_poll_wait);


/**
 * mpi3mr_verify_adapter - verify adapter number is valid
 * @ioc_number: Adapter number
 * @mriocpp: Pointer to hold per adpater instance
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
	else
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
	else
		return 0;
}

/**
 * mpi3mr_app_pel_abort - sends PEL abort request
 * @mrioc: Adapter instance reference
 *
 * This function sends PEL abort request to the firmware through
 * admin request queue.
 *
 * Return: 0 on success, Non-zero on failure
 */
static int mpi3mr_app_pel_abort(struct mpi3mr_ioc *mrioc)
{
	struct mpi3_pel_req_action_abort pel_abort_req;
	struct mpi3_pel_reply *pel_reply;
	int retval = 0;
	u16 pe_log_status;

	if (mrioc->reset_in_progress ||
		mrioc->block_ioctls) {
		dbgprint(mrioc, "%s: reset %d blocked ioctl %d\n",
				__func__, mrioc->reset_in_progress,
				mrioc->block_ioctls);
		return -1;
	}

	memset(&pel_abort_req, 0, sizeof(pel_abort_req));
	mutex_lock(&mrioc->pel_abort_cmd.mutex);
	if (mrioc->pel_abort_cmd.state & MPI3MR_CMD_PENDING) {
		dbgprint(mrioc, "%s: command is in use\n", __func__);
		mutex_unlock(&mrioc->pel_abort_cmd.mutex);
		return -1;
	}
	mrioc->pel_abort_cmd.state = MPI3MR_CMD_PENDING;
	mrioc->pel_abort_cmd.is_waiting = 1;
	mrioc->pel_abort_cmd.callback = NULL;
	pel_abort_req.host_tag = cpu_to_le16(MPI3MR_HOSTTAG_PEL_ABORT);
	pel_abort_req.function = MPI3_FUNCTION_PERSISTENT_EVENT_LOG;
	pel_abort_req.action = MPI3_PEL_ACTION_ABORT;
	pel_abort_req.abort_host_tag = cpu_to_le16(MPI3MR_HOSTTAG_PEL_WAIT);

	mrioc->pel_abort_requested = true;
	init_completion(&mrioc->pel_abort_cmd.done);
	retval = mpi3mr_admin_request_post(mrioc, &pel_abort_req,
	    sizeof(pel_abort_req), 0);
	if (retval) {
		mrioc->pel_abort_requested = false;
		goto out_unlock;
	}

	wait_for_completion_timeout(&mrioc->pel_abort_cmd.done,
	    (MPI3MR_INTADMCMD_TIMEOUT * HZ));
	if (!(mrioc->pel_abort_cmd.state & MPI3MR_CMD_COMPLETE)) {
		mrioc->pel_abort_cmd.is_waiting = 0;
		dbgprint(mrioc, "%s: command timedout\n", __func__);
		if (!(mrioc->pel_abort_cmd.state & MPI3MR_CMD_RESET))
			mpi3mr_soft_reset_handler(mrioc,
			    MPI3MR_RESET_FROM_PELABORT_TIMEOUT, 1);
		retval = -1;
		goto out_unlock;
	}
	if ((mrioc->pel_abort_cmd.ioc_status & MPI3_IOCSTATUS_STATUS_MASK)
	     != MPI3_IOCSTATUS_SUCCESS) {
		dbgprint(mrioc,
		    "%s: command failed, ioc_status(0x%04x) log_info(0x%08x)\n",
		    __func__, (mrioc->pel_abort_cmd.ioc_status &
		    MPI3_IOCSTATUS_STATUS_MASK),
		    mrioc->pel_abort_cmd.ioc_loginfo);
		retval = -1;
		goto out_unlock;
	}
	if (mrioc->pel_abort_cmd.state & MPI3MR_CMD_REPLY_VALID) {
		pel_reply = (struct mpi3_pel_reply *)mrioc->pel_abort_cmd.reply;
		pe_log_status = le16_to_cpu(pel_reply->pe_log_status);
		if (pe_log_status != MPI3_PEL_STATUS_SUCCESS) {
			dbgprint(mrioc,
			    "%s: command failed, pel_status(0x%04x)\n",
			    __func__, pe_log_status);
			retval = -1;
		}
	}

out_unlock:
	mrioc->pel_abort_cmd.state = MPI3MR_CMD_NOTUSED;
	mutex_unlock(&mrioc->pel_abort_cmd.mutex);
	return retval;
}

/**
 * mpi3mr_app_pel_enable - Handler for PEL enable driver IOCTL
 * @mrioc: Adapter instance reference
 * @data_out_buf: User buffer containing PEL enable data
 * @data_out_sz: length of the user buffer.
 *
 * This function is the handler for PEL enable driver IOCTL.
 * Validates the application given class and locale and if
 * requires aborts the existing PEL wait request and/or issues
 * new PEL wait request to the firmware and returns.
 *
 * Return: 0 on success and proper error codes on failure.
 */
static long mpi3mr_app_pel_enable(struct mpi3mr_ioc *mrioc,
	void __user *data_out_buf, uint32_t data_out_sz)
{
	long rval = 0;
	struct mpi3mr_ioctl_out_pel_enable pel_enable;
	u8 tmp_class;
	u16 tmp_locale;

	if (copy_from_user(&pel_enable, data_out_buf, sizeof(pel_enable)))
		return -EFAULT;

	if (pel_enable.pel_class > MPI3_PEL_CLASS_FAULT) {
		dbgprint(mrioc, "%s: out of range class %d sent\n",
			__func__, pel_enable.pel_class);
		return -EINVAL;
	}

	if (mrioc->pel_enabled) {
		if ((mrioc->pel_class <= pel_enable.pel_class) &&
		    !((mrioc->pel_locale & pel_enable.pel_locale) ^
		      pel_enable.pel_locale))
			return 0;
		else {
			pel_enable.pel_locale |= mrioc->pel_locale;
			if (mrioc->pel_class < pel_enable.pel_class)
				pel_enable.pel_class = mrioc->pel_class;

			rval = mpi3mr_app_pel_abort(mrioc);

			if (rval)
				return rval;
		}
	}

	tmp_class = mrioc->pel_class;
	tmp_locale = mrioc->pel_locale;
	mrioc->pel_class = pel_enable.pel_class;
	mrioc->pel_locale = pel_enable.pel_locale;
	mrioc->pel_enabled = true;
	rval = mpi3mr_pel_get_seqnum_post(mrioc, NULL);
	if (rval) {
		mrioc->pel_class = tmp_class;
		mrioc->pel_locale = tmp_locale;
		mrioc->pel_enabled = false;
		dbgprint(mrioc,
		    "%s: pel get sequence number failed, status(%ld)\n",
		    __func__, rval);
	}

	return rval;
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
	else
		return 0;
}

/**
 * mpi3mr_ioctl_adp_reset - Issue controller reset
 * @mrioc: Adapter instance reference
 * @data_out_buf: User buffer with reset type
 * @data_out_sz: length of the user buffer.
 *
 * This function identifies the user provided reset type and
 * issues approporiate reset to the controller, wait for that
 * to complete, reinitialize the controller and then returns
 *
 * Return: 0 on success and proper error codes on failure
 */
static long mpi3mr_ioctl_adp_reset(struct mpi3mr_ioc *mrioc,
		void __user *data_out_buf, uint32_t data_out_sz)
{
	long rval = 0;
	struct mpi3mr_ioctl_adp_reset adpreset;

	if (copy_from_user(&adpreset, data_out_buf, sizeof(adpreset)))
		return -EFAULT;

	switch (adpreset.reset_type) {
	case MPI3MR_IOCTL_ADPRESET_SOFT:
		rval = mpi3mr_soft_reset_handler(mrioc,
				MPI3MR_RESET_FROM_IOCTL, 0);
		dbgprint(mrioc, "reset_type (0x%x) error code 0x%lx\n",
			 adpreset.reset_type, rval);
		break;
	case MPI3MR_IOCTL_ADPRESET_DIAG_FAULT:
		rval = mpi3mr_diagfault_reset_handler(mrioc,
				MPI3MR_RESET_FROM_IOCTL);
		dbgprint(mrioc, "reset_type (0x%x) error code 0x%lx\n",
			 adpreset.reset_type, rval);
		break;
	default:
		dbgprint(mrioc, "Unknown reset_type(0x%x) issued\n",
			 adpreset.reset_type);
	}

	return rval;
}

/**
 * mpi3mr_populate_adpinfo - Get adapter info IOCTL handler
 * @mrioc: Adapter instance reference
 * @data_in_buf: User buffer to hold adapter information
 * @data_in_sz: length of the user buffer.
 *
 * This function provides adpater information for the given
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
	else
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
	case MPI3MR_DRVIOCTL_OPCODE_PELENABLE:
		rval = mpi3mr_app_pel_enable(mrioc, karg.data_out_buf,
		    karg.data_out_size);
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
 * @num_datasges: Number of data buffers in the list
 *
 * This function places the DMA address of the given buffers in
 * proper format as SGEs in the given MPI request.
 *
 * Return: Nothing
 */
static void mpi3mr_ioctl_build_sgl(u8 *mpi_req, uint32_t sgl_offset,
		struct mpi3mr_buf_map *dma_buffers,
		u8 bufcnt, bool is_rmc, bool is_rmr, u8 num_datasges)
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
	if (!num_datasges) {
		mpi3mr_build_zero_len_sge(sgl);
		return;
	}
	for (; count < bufcnt; count++, dma_buff++) {
		if (dma_buff->data_dir == DMA_BIDIRECTIONAL)
			continue;
		if (num_datasges == 1 || !is_rmc)
			mpi3mr_add_sg_single(sgl, sgl_flags_last,
					     dma_buff->kern_buf_len,
					     dma_buff->kern_buf_dma);
		else
			mpi3mr_add_sg_single(sgl, sgl_flags,
					     dma_buff->kern_buf_len,
					     dma_buff->kern_buf_dma);
		sgl += sizeof(struct mpi3_sge_common);
		num_datasges--;
	}
}

/**
 * mpi3mr_get_nvme_data_fmt - returns the NVMe data format
 * @nvme_encap_request: NVMe encapsulated MPI request
 *
 * This function returns the type of the data format specified
 * in user provided NVMe command in NVMe encapsulated request.
 *
 * Return: Data format of the NVMe command (PRP/SGL etc)
 */
static u8 mpi3mr_get_nvme_data_fmt(
	struct mpi3_nvme_encapsulated_request *nvme_encap_request)
{
	return (u8)((nvme_encap_request->command[0] & 0xc000) >> 14);
}

/**
 * mpi3mr_build_nvme_sgl - SGL constructor for NVME
 *				   encapsulated request
 * @mrioc: Adapter instance reference
 * @nvme_encap_request: NVMe encapsulated MPI request
 * @dma_buffers: DMA address of the buffers to be placed in sgl
 * @bufcnt: Number of DMA buffers
 *
 * This function places the DMA address of the given buffers in
 * proper format as SGEs in the given NVMe encapsulated request.
 *
 * Return: 0 on success, -1 on failure
 */
static int mpi3mr_build_nvme_sgl(struct mpi3mr_ioc *mrioc,
	struct mpi3_nvme_encapsulated_request *nvme_encap_request,
	struct mpi3mr_buf_map *dma_buffers, u8 bufcnt)
{
	struct mpi3mr_nvme_pt_sge *nvme_sgl;
	u64 sgl_ptr, sgemod_mask, sgemod_val;
	u8 count;
	size_t length = 0;
	struct mpi3mr_buf_map *dma_buff = dma_buffers;

	/*
	 * Not all commands require a data transfer. If no data, just return
	 * without constructing any sgl.
	 */
	for (count = 0; count < bufcnt; count++, dma_buff++) {
		if ((dma_buff->data_dir == DMA_TO_DEVICE) ||
		    (dma_buff->data_dir == DMA_FROM_DEVICE)) {
			sgl_ptr = (u64)dma_buff->kern_buf_dma;
			length = dma_buff->kern_buf_len;
			break;
		}
	}
	if (!length)
		return 0;

	sgemod_mask = ((u64)((mrioc->facts.sge_mod_mask) <<
				mrioc->facts.sge_mod_shift) << 32);
	sgemod_val = ((u64)(mrioc->facts.sge_mod_value) <<
				mrioc->facts.sge_mod_shift) << 32;

	if (sgl_ptr & sgemod_mask) {
		dbgprint(mrioc,
		    "%s: SGL address collides with SGE modifier\n",
		    __func__);
		return -1;
	}

	sgl_ptr &= ~sgemod_mask;
	sgl_ptr |= sgemod_val;
	nvme_sgl = (struct mpi3mr_nvme_pt_sge *)
	    ((u8 *)(nvme_encap_request->command) + MPI3MR_NVME_CMD_SGL_OFFSET);
	memset(nvme_sgl, 0, sizeof(struct mpi3mr_nvme_pt_sge));
	nvme_sgl->base_addr = sgl_ptr;
	nvme_sgl->length = length;

	return 0;
}

/**
 * mpi3mr_build_nvme_prp - PRP constructor for NVME
 *			       encapsulated request
 * @mrioc: Adapter instance reference
 * @nvme_encap_request: NVMe encapsulated MPI request
 * @dma_buffers: DMA address of the buffers to be placed in SGL
 * @bufcnt: Number of DMA buffers
 *
 * This function places the DMA address of the given buffers in
 * proper format as PRP entries in the given NVMe encapsulated
 * request.
 *
 * Return: 0 on success, -1 on failure
 */
static int mpi3mr_build_nvme_prp(struct mpi3mr_ioc *mrioc,
    struct mpi3_nvme_encapsulated_request *nvme_encap_request,
    struct mpi3mr_buf_map *dma_buffers, u8 bufcnt)
{
	int prp_size = MPI3MR_NVME_PRP_SIZE;
	__le64 *prp_entry, *prp1_entry, *prp2_entry, *prp_page;
	dma_addr_t prp_entry_dma, prp_page_dma, dma_addr;
	u32 offset, entry_len, dev_pgsz, page_mask_result, page_mask;
	size_t length = 0;
	u8 count;
	struct mpi3mr_buf_map *dma_buff;
	struct mpi3mr_tgt_dev *tgtdev;
	u64 sgemod_mask, sgemod_val;
	u16 dev_handle;

	dma_buff = dma_buffers;
	dev_handle = nvme_encap_request->dev_handle;

	/*
	 * Not all commands require a data transfer. If no data, just return
	 * without constructing any PRP.
	 */
	for (count = 0; count < bufcnt; count++, dma_buff++) {
		if ((dma_buff->data_dir == DMA_TO_DEVICE) ||
		    (dma_buff->data_dir == DMA_FROM_DEVICE)) {
			dma_addr = dma_buff->kern_buf_dma;
			length = dma_buff->kern_buf_len;
			break;
		}
	}
	if (!length)
		return 0;

	tgtdev = mpi3mr_get_tgtdev_by_handle(mrioc, dev_handle);
	if (!tgtdev) {
		dbgprint(mrioc, "%s: invalid device handle 0x%04x\n",
			__func__, dev_handle);
		return -1;
	}
	if (tgtdev->dev_spec.pcie_inf.pgsz == 0) {
		dbgprint(mrioc,
		    "%s: NVME device page size is zero for handle 0x%04x\n",
		    __func__, dev_handle);
		mpi3mr_tgtdev_put(tgtdev);
		return -1;
	}
	dev_pgsz = 1 << (tgtdev->dev_spec.pcie_inf.pgsz);
	mpi3mr_tgtdev_put(tgtdev);

	mrioc->nvme_encap_prp_sz = 0;
	mrioc->nvme_encap_prp_list = dma_alloc_coherent(&mrioc->pdev->dev,
					dev_pgsz,
					&mrioc->nvme_encap_prp_list_dma,
					GFP_KERNEL);

	if (!mrioc->nvme_encap_prp_list)
		return -1;

	mrioc->nvme_encap_prp_sz = dev_pgsz;
	/*
	 * Set pointers to PRP1 and PRP2, which are in the NVMe command.
	 * PRP1 is located at a 24 byte offset from the start of the NVMe
	 * command.  Then set the current PRP entry pointer to PRP1.
	 */
	prp1_entry = (__le64 *)((u8 *)(nvme_encap_request->command) +
						MPI3MR_NVME_CMD_PRP1_OFFSET);
	prp2_entry = (__le64 *)((u8 *)(nvme_encap_request->command) +
						MPI3MR_NVME_CMD_PRP2_OFFSET);
	prp_entry = prp1_entry;
	/*
	 * For the PRP entries, use the specially allocated buffer of
	 * contiguous memory.
	 */
	prp_page = (__le64 *)mrioc->nvme_encap_prp_list;
	prp_page_dma = mrioc->nvme_encap_prp_list_dma;

	/*
	 * Check if we are within 1 entry of a page boundary we don't
	 * want our first entry to be a PRP List entry.
	 */
	page_mask = dev_pgsz - 1;
	page_mask_result = (uintptr_t)((u8 *)prp_page + prp_size) & page_mask;
	if (!page_mask_result) {
		ioc_err(mrioc, "%s: PRP page is not page aligned\n", __func__);
		goto err_out;
	}

	/*
	 * Set PRP physical pointer, which initially points to the current PRP
	 * DMA memory page.
	 */
	prp_entry_dma = prp_page_dma;
	sgemod_mask = ((u64)((mrioc->facts.sge_mod_mask) <<
				mrioc->facts.sge_mod_shift) << 32);
	sgemod_val = ((u64)(mrioc->facts.sge_mod_value) <<
				mrioc->facts.sge_mod_shift) << 32;

	/* Loop while the length is not zero. */
	while (length) {
		page_mask_result = (prp_entry_dma + prp_size) & page_mask;
		if (!page_mask_result && (length >  dev_pgsz)) {
			dbgprint(mrioc,
			    "%s: single PRP page is not sufficient\n",
			    __func__);
			goto err_out;
		}

		/* Need to handle if entry will be part of a page. */
		offset = dma_addr & page_mask;
		entry_len = dev_pgsz - offset;

		if (prp_entry == prp1_entry) {
			/*
			 * Must fill in the first PRP pointer (PRP1) before
			 * moving on.
			 */
			*prp1_entry = cpu_to_le64(dma_addr);
			if (*prp1_entry & sgemod_mask) {
				dbgprint(mrioc,
				    "%s: PRP1 address collides with SGE modifier\n",
				    __func__);
				goto err_out;
			}
			*prp1_entry &= ~sgemod_mask;
			*prp1_entry |= sgemod_val;

			/*
			 * Now point to the second PRP entry within the
			 * command (PRP2).
			 */
			prp_entry = prp2_entry;
		} else if (prp_entry == prp2_entry) {
			/*
			 * Should the PRP2 entry be a PRP List pointer or just
			 * a regular PRP pointer?  If there is more than one
			 * more page of data, must use a PRP List pointer.
			 */
			if (length > dev_pgsz) {
				/*
				 * PRP2 will contain a PRP List pointer because
				 * more PRP's are needed with this command. The
				 * list will start at the beginning of the
				 * contiguous buffer.
				 */
				*prp2_entry = cpu_to_le64(prp_entry_dma);
				if (*prp2_entry & sgemod_mask) {
					dbgprint(mrioc,
					    "%s: PRP list address collides with SGE modifier\n",
					    __func__);
					goto err_out;
				}
				*prp2_entry &= ~sgemod_mask;
				*prp2_entry |= sgemod_val;

				/*
				 * The next PRP Entry will be the start of the
				 * first PRP List.
				 */
				prp_entry = prp_page;
				continue;
			} else {
				/*
				 * After this, the PRP Entries are complete.
				 * This command uses 2 PRP's and no PRP list.
				 */
				*prp2_entry = cpu_to_le64(dma_addr);
				if (*prp2_entry & sgemod_mask) {
					dbgprint(mrioc,
					    "%s: PRP2 collides with SGE modifier\n",
					    __func__);
					goto err_out;
				}
				*prp2_entry &= ~sgemod_mask;
				*prp2_entry |= sgemod_val;
			}
		} else {
			/*
			 * Put entry in list and bump the addresses.
			 *
			 * After PRP1 and PRP2 are filled in, this will fill in
			 * all remaining PRP entries in a PRP List, one per
			 * each time through the loop.
			 */
			*prp_entry = cpu_to_le64(dma_addr);
			if (*prp1_entry & sgemod_mask) {
				dbgprint(mrioc,
				    "%s: PRP address collides with SGE modifier\n",
				    __func__);
				goto err_out;
			}
			*prp_entry &= ~sgemod_mask;
			*prp_entry |= sgemod_val;
			prp_entry++;
			prp_entry_dma++;
		}

		/*
		 * Bump the phys address of the command's data buffer by the
		 * entry_len.
		 */
		dma_addr += entry_len;

		/* decrement length accounting for last partial page. */
		if (entry_len > length)
			length = 0;
		else
			length -= entry_len;
	}
	return 0;
err_out:
	if (mrioc->nvme_encap_prp_list) {
		dma_free_coherent(&mrioc->pdev->dev, mrioc->nvme_encap_prp_sz,
		    mrioc->nvme_encap_prp_list, mrioc->nvme_encap_prp_list_dma);
		mrioc->nvme_encap_prp_list = NULL;
	}
	return -1;
}


/**
 * mpi3mr_ioctl_process_mpt_cmds - MPI Pass through IOCTL handler
 * @mrioc: Adapter instance reference
 * @arg: User data payload buffer for the IOCTL
 *
 * This function is the top level handler for MPI Pass through
 * IOCTL, this does basic validation of the input data buffers,
 * identifies the given buffer types and MPI command, allocates
 * DMAable memory for user given buffers, construstcs SGL
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
	u8 count, bufcnt, din_cnt = 0, dout_cnt = 0, nvme_fmt;
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

	if (mpi_header->function == MPI3_FUNCTION_NVME_ENCAPSULATED) {
		nvme_fmt = mpi3mr_get_nvme_data_fmt(
			(struct mpi3_nvme_encapsulated_request *)mpi_req);
		if (nvme_fmt == MPI3MR_NVME_DATA_FORMAT_PRP) {
			if (mpi3mr_build_nvme_prp(mrioc,
			    (struct mpi3_nvme_encapsulated_request *)mpi_req,
			    dma_buffers, bufcnt)) {
				rval = -ENOMEM;
				mutex_unlock(&mrioc->ioctl_cmds.mutex);
				goto out;
			}
		} else if (nvme_fmt == MPI3MR_NVME_DATA_FORMAT_SGL1 ||
			nvme_fmt == MPI3MR_NVME_DATA_FORMAT_SGL2) {
			if (mpi3mr_build_nvme_sgl(mrioc,
			    (struct mpi3_nvme_encapsulated_request *)mpi_req,
			    dma_buffers, bufcnt)) {
				rval = -EINVAL;
				mutex_unlock(&mrioc->ioctl_cmds.mutex);
				goto out;
			}
		} else {
			dbgprint(mrioc,
			    "%s:invalid NVMe command format\n", __func__);
			rval = -EINVAL;
			mutex_unlock(&mrioc->ioctl_cmds.mutex);
			goto out;
		}
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
	long rval = 0;

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
		rval = -EINVAL;
		pr_err("%s:Unsupported ioctl cmd (0x%08x)\n", __func__, cmd);
		break;
	}
	return rval;
}

/**
 * mpi3mr_app_send_aen - Notify applications about an AEN
 * @mrioc: Adapter instance reference
 *
 * Sends async signal SIGIO to indicate there is an async event
 * from the firmware to the event monitoring applications.
 *
 * Return:Nothing
 */
void mpi3mr_app_send_aen(struct mpi3mr_ioc *mrioc)
{
	dbgprint(mrioc, "%s: invoked\n", __func__);
	if (mpi3mr_app_async_queue) {
		dbgprint(mrioc, "%s: sending signal\n", __func__);
		kill_fasync(&mpi3mr_app_async_queue, SIGIO, POLL_IN);
	}
}

/**
 * mpi3mr_app_poll - Obsolete willbe removed
 *
 * Return:POLLIN | POLLRDNORM
 */
static unsigned int mpi3mr_app_poll(struct file *filep, poll_table *wait)
{
	poll_wait(filep, &_app_poll_wait, wait);
	pr_info("Returning POLLIN | POLLRDNORM from poll()\n");
	return POLLIN | POLLRDNORM;
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
	.poll = mpi3mr_app_poll,
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

	init_waitqueue_head(&_app_poll_wait);
}

/**
 * mpi3mr_app_exit - Character driver interface cleanup function
 *
 */
void mpi3mr_app_exit(void)
{
	misc_deregister(&mpi3mr_app_dev);
}
