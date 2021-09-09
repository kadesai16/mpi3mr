/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Driver for Broadcom MPI3 Storage Controllers
 *
 * Copyright (C) 2017-2021 Broadcom Inc.
 *  (mailto: mpi3mr-linuxdrv.pdl@broadcom.com)
 *
 */

#ifndef MPI3MR_APP_INTFC_H_INCLUDED
#define MPI3MR_APP_INTFC_H_INCLUDED

#ifdef __KERNEL__
#include <linux/miscdevice.h>
#endif

/*Definitions for IOCTL commands*/
#ifndef MPI3MR_MINOR
#define MPI3MR_MINOR			(MISC_DYNAMIC_MINOR - 1)
#endif
#define MPI3MR_DEV_NAME			"mpi3mrctl"
#define MPI3MR_MAGIC_NUMBER		'B'

#define MPI3MR_IOCTL_VERSION		0x05

#define MPI3MR_IOCTL_DEFAULT_TIMEOUT	(60) /*seconds*/

#define MPI3MR_IOCTL_ADPTYPE_UNKNOWN		0
#define MPI3MR_IOCTL_ADPTYPE_AVGFAMILY		1

#define MPI3MR_IOCTL_ADPRESET_UNKNOWN		0
#define MPI3MR_IOCTL_ADPRESET_SOFT		1
#define MPI3MR_IOCTL_ADPRESET_DIAG_FAULT	2

#define MPI3MR_IOCTL_LOGDATA_MAX_ENTRIES	400
#define MPI3MR_IOCTL_LOGDATA_ENTRY_HEADER_SZ	4

#define MPI3MR_DRVIOCTL_OPCODE_UNKNOWN			0
#define MPI3MR_DRVIOCTL_OPCODE_ADPINFO			1
#define MPI3MR_DRVIOCTL_OPCODE_ADPRESET			2
#define MPI3MR_DRVIOCTL_OPCODE_ALLTGTDEVINFO		4
#define MPI3MR_DRVIOCTL_OPCODE_GETCHGCNT		5
#define MPI3MR_DRVIOCTL_OPCODE_LOGDATAENABLE		6
#define MPI3MR_DRVIOCTL_OPCODE_PELENABLE		7
#define MPI3MR_DRVIOCTL_OPCODE_GETLOGDATA		8
#define MPI3MR_DRVIOCTL_OPCODE_QUERY_HDB		9
#define MPI3MR_DRVIOCTL_OPCODE_REPOST_HDB		10
#define MPI3MR_DRVIOCTL_OPCODE_UPLOAD_HDB		11
#define MPI3MR_DRVIOCTL_OPCODE_REFRESH_HDB_TRIGGERS	12


#define MPI3MR_IOCTL_BUFTYPE_UNKNOWN		0
#define MPI3MR_IOCTL_BUFTYPE_RAIDMGMT_CMD	1
#define MPI3MR_IOCTL_BUFTYPE_RAIDMGMT_RESP	2
#define MPI3MR_IOCTL_BUFTYPE_DATA_IN		3
#define MPI3MR_IOCTL_BUFTYPE_DATA_OUT		4
#define MPI3MR_IOCTL_BUFTYPE_MPI_REPLY		5
#define MPI3MR_IOCTL_BUFTYPE_ERR_RESPONSE	6

#define MPI3MR_IOCTL_MPI_REPLY_BUFTYPE_UNKNOWN	0
#define MPI3MR_IOCTL_MPI_REPLY_BUFTYPE_STATUS	1
#define MPI3MR_IOCTL_MPI_REPLY_BUFTYPE_ADDRESS	2

#define MPI3MR_HDB_BUFTYPE_UNKNOWN		0
#define MPI3MR_HDB_BUFTYPE_TRACE		1
#define MPI3MR_HDB_BUFTYPE_FIRMWARE		2
#define MPI3MR_HDB_BUFTYPE_RESERVED		3

#define MPI3MR_HDB_BUFSTATUS_UNKNOWN		0
#define MPI3MR_HDB_BUFSTATUS_NOT_ALLOCATED	1
#define MPI3MR_HDB_BUFSTATUS_POSTED_UNPAUSED	2
#define MPI3MR_HDB_BUFSTATUS_POSTED_PAUSED	3
#define MPI3MR_HDB_BUFSTATUS_RELEASED		4

#define MPI3MR_HDB_TRIGGER_TYPE_UNKNOWN		0
#define MPI3MR_HDB_TRIGGER_TYPE_DIAGFAULT	1
#define MPI3MR_HDB_TRIGGER_TYPE_ELEMENT		2
#define MPI3MR_HDB_TRIGGER_TYPE_MASTER		3

/**
 * struct mpi3mr_adp_info - Adapter information IOCTL
 * data returned by the driver.
 *
 * @adp_type: Adapter type
 * @rsvd1: Reserved
 * @pci_dev_id: PCI device ID of the adapter
 * @pci_dev_hw_rev: PCI revision of the adapter
 * @pci_subsys_dev_id: PCI subsystem device ID of the adapter
 * @pci_subsys_ven_id: PCI subsystem vendor ID of the adapter
 * @pci_dev: PCI device
 * @pci_func: PCI function
 * @pci_bus: PCI bus
 * @pci_seg_id: PCI segment ID
 * @ioctl_ver: version of the IOCTL definition
 * @rsvd2: Reserved
 * @driver_info: Driver Information (Version/Name)
 */
struct mpi3mr_adp_info {
	uint32_t adp_type;
	uint32_t rsvd1;
	uint32_t pci_dev_id;
	uint32_t pci_dev_hw_rev;
	uint32_t pci_subsys_dev_id;
	uint32_t pci_subsys_ven_id;
	uint32_t pci_dev:5;
	uint32_t pci_func:3;
	uint32_t pci_bus:24;
	uint32_t pci_seg_id;
	uint32_t ioctl_ver;
	uint32_t rsvd2[3];
	struct mpi3_driver_info_layout driver_info;
};

/**
 * struct mpi3mr_buf_map -  local structure to
 * track kernel and user buffers associated with an IOCTL
 * structure.
 *
 * @user_buf: User buffer virtual address
 * @kern_buf: Kernel buffer virtual address
 * @kern_buf_dma: Kernel buffer DMA address
 * @user_buf_len: User buffer length
 * @kern_buf_len: Kernel buffer length
 * @data_dir: Data direction.
 */
struct mpi3mr_buf_map {
	void __user *user_buf;
	void *kern_buf;
	dma_addr_t kern_buf_dma;
	u32 user_buf_len;
	u32 kern_buf_len;
	u8 data_dir;
};

/**
 * struct mpi3mr_ioctl_adp_reset - Adapter reset IOCTL
 * payload data to the driver.
 *
 * @reset_type: Reset type
 * @rsvd1: Reserved
 * @rsvd2: Reserved
 */
struct mpi3mr_ioctl_adp_reset {
	uint8_t reset_type;
	uint8_t rsvd1;
	uint16_t rsvd2;
};

/**
 * struct mpi3mr_change_count - Topology change count
 * returned by the driver.
 *
 * @change_count: Topology change count
 * @rsvd: Reserved
 */
struct mpi3mr_change_count {
	uint16_t change_count;
	uint16_t rsvd;
};

/**
 * struct mpi3mr_device_map_info - Target device mapping
 * information
 *
 * @handle: Firmware device handle
 * @perst_id: Persistent ID assigned by the firmware
 * @target_id: Target ID assigned by the driver
 * @bus_id: Bus ID assigned by the driver
 * @rsvd1: Reserved
 * @rsvd2: Reserved
 */
struct mpi3mr_device_map_info {
	uint16_t handle;
	uint16_t perst_id;
	uint32_t target_id;
	uint8_t bus_id;
	uint8_t rsvd1;
	uint16_t rsvd2;
};

/**
 * struct mpi3mr_all_tgt_info - Target device mapping
 * information returned by the driver
 *
 * @num_devices: The number of devices in driver's inventory
 * @rsvd1: Reserved
 * @rsvd2: Reserved
 * @dmi: Variable length array of mapping information of targets
 */
struct mpi3mr_all_tgt_info {
	uint16_t num_devices; //The number of devices in driver's inventory
	uint16_t rsvd1;
	uint32_t rsvd2;
	struct mpi3mr_device_map_info dmi[1]; //Variable length Array
};

/**
 * struct mpi3mr_logdata_enable - Number of log data
 * entries saved by the driver returned as payload data for
 * enable logdata IOCTL by the driver.
 *
 * @max_entries: Number of log data entries cached by the driver
 * @rsvd: Reserved
 */
struct mpi3mr_logdata_enable {
	uint16_t max_entries;
	uint16_t rsvd;
};

/**
 * struct mpi3mr_ioctl_out_pel_enable - PEL enable IOCTL payload
 * data to the driver.
 *
 * @pel_locale: PEL locale to the firmware
 * @pel_class: PEL class to the firmware
 * @rsvd: Reserved
 */
struct mpi3mr_ioctl_out_pel_enable {
	uint16_t pel_locale;
	uint8_t pel_class;
	uint8_t rsvd;
};

/**
 * struct mpi3mr_logdata_entry - Log data entry cached by the
 * driver.
 *
 * @valid_entry: Is the entry valid
 * @rsvd1: Reserved
 * @rsvd2: Reserved
 * @data: Log entry data of controller specific size
 */
struct mpi3mr_logdata_entry {
	uint8_t valid_entry;
	uint8_t rsvd1;
	uint16_t rsvd2;
	uint8_t data[1]; //Variable length Array
};

/**
 * struct mpi3mr_ioctl_in_log_data - Log data entries saved by
 * the driver returned as payload data for Get logdata IOCTL
 * by the driver.
 *
 * @entry: Log data entry
 */
struct mpi3mr_ioctl_in_log_data {
	struct mpi3mr_logdata_entry entry[1]; //Variable length Array
};


/**
 * struct mpi3mr_ioctl_drv_cmd -  Generic IOCTL payload data
 * structure for all driver specific IOCTLS .
 *
 * @mrioc_id: Controller ID
 * @opcode: Driver IOCTL specific opcode
 * @rsvd1: Reserved
 * @rsvd2: Reserved
 * @data_in_buf: User data buffer pointer of data from driver
 * @data_out_buf: User data buffer pointer of data to driver
 * @data_in_size: Data in buffer size
 * @data_out_size: Data out buffer size
 */
struct mpi3mr_ioctl_drv_cmd {
	uint8_t mrioc_id;
	uint8_t opcode;
	uint16_t rsvd1;
	uint32_t rsvd2;
#ifdef __KERNEL__
	void __user *data_in_buf;
	void __user *data_out_buf;
#else
	void *data_in_buf;
	void *data_out_buf;
#endif
	uint32_t data_in_size;
	uint32_t data_out_size;
};

/**
 * struct mpi3mr_ioctl_reply_buf - MPI reply buffer returned
 * for MPI Passthrough IOCTLs .
 *
 * @mpi_reply_type: Type of MPI reply
 * @rsvd1: Reserved
 * @rsvd2: Reserved
 * @ioctl_reply_buf: Variable Length buffer based on mpirep type
 */
struct mpi3mr_ioctl_reply_buf {
	uint8_t mpi_reply_type;
	uint8_t rsvd1;
	uint16_t rsvd2;
	uint8_t ioctl_reply_buf[1]; /*Variable Length buffer based on mpirep type*/
};


/**
 * struct mpi3mr_buf_entry - User buffer descriptor for MPI
 * Passthrough IOCTLs.
 *
 * @buf_type: Buffer type
 * @rsvd1: Reserved
 * @rsvd2: Reserved
 * @buf_len: Buffer length
 * @buffer: User space buffer address
 */
struct mpi3mr_buf_entry {
	uint8_t buf_type;
	uint8_t rsvd1;
	uint16_t rsvd2;
	uint32_t buf_len;
#ifdef __KERNEL__
	void __user *buffer;
#else
	void *buffer;
#endif
};

/**
 * struct mpi3mr_ioctl_buf_entry_list - list of user buffer
 * descriptor for MPI Passthrough IOCTLs.
 *
 * @num_of_entries: Number of buffer descriptors
 * @rsvd1: Reserved
 * @rsvd2: Reserved
 * @rsvd3: Reserved
 * @buf_entry: Variable length array of buffer descriptors
 */
struct mpi3mr_ioctl_buf_entry_list {
	uint8_t num_of_entries;
	uint8_t rsvd1;
	uint16_t rsvd2;
	uint32_t rsvd3;
	struct mpi3mr_buf_entry buf_entry[1]; //Variable length Array
};

/**
 * struct mpi3mr_ioctl_mptcmd -  Generic IOCTL payload data
 * structure for all MPI Passthrough IOCTLS .
 *
 * @mrioc_id: Controller ID
 * @rsvd1: Reserved
 * @timeout: MPI command timeout
 * @rsvd2: Reserved
 * @mpi_msg_size: MPI message size
 * @mpi_msg_buf: MPI message
 * @buf_entry_list: Buffer descriptor list
 * @buf_entry_list_size: Buffer descriptor list size
 */
struct mpi3mr_ioctl_mptcmd {
	uint8_t mrioc_id;
	uint8_t rsvd1;
	uint16_t timeout;
	uint16_t rsvd2;
	uint16_t mpi_msg_size;
#ifdef __KERNEL__
	void __user *mpi_msg_buf;
	void __user *buf_entry_list;
#else
	void *mpi_msg_buf;
	void *buf_entry_list;
#endif
	uint32_t buf_entry_list_size;
};

#define MPI3MRDRVCMD	_IOWR(MPI3MR_MAGIC_NUMBER, 1, \
	struct mpi3mr_ioctl_drv_cmd)
#define MPI3MRMPTCMD	_IOWR(MPI3MR_MAGIC_NUMBER, 2, \
	struct mpi3mr_ioctl_mptcmd)

#endif
