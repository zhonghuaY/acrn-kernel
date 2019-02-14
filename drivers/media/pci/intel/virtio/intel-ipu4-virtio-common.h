/* SPDX-License-Identifier: (BSD-3-Clause OR GPL-2.0) */
/*
 * Copyright (C) 2018 Intel Corporation
 */

#ifndef __IPU4_VIRTIO_COMMON_H__
#define __IPU4_VIRTIO_COMMON_H__

/*
 * CWP uses physicall addresses for memory sharing,
 * so size of one page ref will be 64-bits
 */

#define REFS_PER_PAGE (PAGE_SIZE/sizeof(u64))
/* Defines size of requests circular buffer */
#define REQ_RING_SIZE 128
#define MAX_NUMBER_OF_OPERANDS 64
#define MAX_ENTRY_FE 7
#define MAX_STREAM_DEVICES 64
#define MAX_PIPELINE_DEVICES 1
#define MAX_ISYS_VIRT_STREAM 35

#define phys_to_page(x) pfn_to_page((x) >> PAGE_SHIFT)

enum virio_queue_type {
      IPU_VIRTIO_QUEUE_0 = 0,
      IPU_VIRTIO_QUEUE_1,
      IPU_VIRTIO_QUEUE_MAX
};

struct ipu4_virtio_req {
	unsigned int req_id;
	unsigned int stat;
	unsigned int cmd;
	unsigned int func_ret;
	unsigned int op[MAX_NUMBER_OF_OPERANDS];
	wait_queue_head_t *wait;
	bool completed;
	u64 payload;
	struct file *be_fh;
};
struct test_payload {
	unsigned int data1;
	long int data2;
	char name[256];
};
/*Not used*/
struct ipu4_virtio_resp {
	unsigned int resp_id;
	unsigned int stat;
	unsigned int cmd;
	unsigned int op[MAX_NUMBER_OF_OPERANDS];
};

/*Not used*/
struct ipu4_virtio_fe_info {
	struct ipu4_virtio_be_priv *priv;
	int client_id;
	int vmid;
	int max_vcpu;
	struct vhm_request *req_buf;
};

/*Not used*/
struct ipu4_virtio_fe_info_entry {
	struct ipu4_virtio_fe_info *info;
	struct hlist_node node;
};

struct ipu4_bknd_ops {
	/* backed initialization routine */
	int (*init)(void);

	/* backed cleanup routine */
	void (*cleanup)(void);

	/* retreiving id of current virtual machine */
	int (*get_vm_id)(void);

	int (*send_req)(int, struct ipu4_virtio_req *, int, int);
};

struct ipu4_virtio_ctx {
	/* VM(domain) id of current VM instance */
	int domid;

	/* backend ops - hypervisor specific */
	struct ipu4_bknd_ops *bknd_ops;

	/* flag that shows whether backend is initialized */
	bool initialized;

	/* device global lock */
	struct mutex lock;
};

enum intel_ipu4_virtio_command {
	IPU4_CMD_DEVICE_OPEN = 0x1,
	IPU4_CMD_DEVICE_CLOSE,
	IPU4_CMD_STREAM_ON,
	IPU4_CMD_STREAM_OFF,
	IPU4_CMD_GET_BUF,
	IPU4_CMD_PUT_BUF,
	IPU4_CMD_SET_FORMAT,
	IPU4_CMD_ENUM_NODES,
	IPU4_CMD_ENUM_LINKS,
	IPU4_CMD_SETUP_PIPE,
	IPU4_CMD_SET_FRAMEFMT,
	IPU4_CMD_GET_FRAMEFMT,
	IPU4_CMD_GET_SUPPORTED_FRAMEFMT,
	IPU4_CMD_SET_SELECTION,
	IPU4_CMD_GET_SELECTION,
	IPU4_CMD_POLL,
	IPU4_CMD_PIPELINE_OPEN,
	IPU4_CMD_PIPELINE_CLOSE,
	IPU4_CMD_PSYS_MAPBUF,
	IPU4_CMD_PSYS_UNMAPBUF,
	IPU4_CMD_PSYS_QUERYCAP,
	IPU4_CMD_PSYS_GETBUF,
	IPU4_CMD_PSYS_PUTBUF,
	IPU4_CMD_PSYS_QCMD,
	IPU4_CMD_PSYS_DQEVENT,
	IPU4_CMD_PSYS_GET_MANIFEST,
	IPU4_CMD_PSYS_OPEN,
	IPU4_CMD_PSYS_CLOSE,
	IPU4_CMD_PSYS_POLL,
	IPU4_CMD_GET_N
};

enum intel_ipu4_virtio_req_feedback {
	IPU4_REQ_ERROR = -1,
	IPU4_REQ_PROCESSED,
	IPU4_REQ_PENDING,
	IPU4_REQ_NOT_RESPONDED
};

struct ipu4_virtio_ring {
	/* Buffer allocated for keeping ring entries */
	u64 *buffer;

	/* Index pointing to next free element in ring */
	int head;

	/* Index pointing to last released element in ring */
	int tail;

	/* Total number of elements that ring can contain */
	int ring_size;

	/* Number of location in ring has been used */
	unsigned int used;

	/* Multi thread sync */
	spinlock_t lock;
};

/* Create the ring buffer with given size */
int ipu4_virtio_ring_init(struct ipu4_virtio_ring *ring,
			  int ring_size);

/* Frees the ring buffers */
void ipu4_virtio_ring_free(struct ipu4_virtio_ring *ring);

/* Add a buffer to ring */
int ipu4_virtio_ring_push(struct ipu4_virtio_ring *ring, void *data);

/* Grab a buffer from ring */
void *ipu4_virtio_ring_pop(struct ipu4_virtio_ring *ring);

extern struct ipu4_bknd_ops ipu4_virtio_bknd_ops;

void ipu4_virtio_fe_table_init(void);

int ipu4_virtio_fe_add(struct ipu4_virtio_fe_info *fe_info);

struct ipu4_virtio_fe_info *ipu4_virtio_fe_find(int client_id);

struct ipu4_virtio_fe_info *ipu4_virtio_fe_find_by_vmid(int vmid);

#endif
