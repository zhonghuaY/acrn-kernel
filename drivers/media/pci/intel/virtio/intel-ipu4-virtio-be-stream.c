// SPDX-License-Identifier: (BSD-3-Clause OR GPL-2.0)
/*
 * Copyright (C) 2018 Intel Corporation
 */

#include <linux/kernel.h>
#include <linux/file.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/poll.h>
#include <linux/hashtable.h>
#include <linux/pagemap.h>

#include <media/ici.h>
#include <linux/vhm/acrn_vhm_mm.h>
#include "./ici/ici-isys-stream-device.h"
#include "./ici/ici-isys-stream.h"
#include "./ici/ici-isys-frame-buf.h"
#include "intel-ipu4-virtio-be-stream.h"
#include "intel-ipu4-virtio-be.h"

#define MAX_SIZE 6 // max 2^6
#define POLL_WAIT 20000 //20s

#define dev_to_stream(dev) \
	container_of(dev, struct ici_isys_stream, strm_dev)

DECLARE_HASHTABLE(STREAM_NODE_HASH, MAX_SIZE);
static bool hash_initialised;
static spinlock_t stream_node_hash_lock;

struct stream_node {
	int client_id;
	struct file *f;
	struct hlist_node node;
};

void cleanup_stream(void)
{
	struct stream_node *sn = NULL;
	unsigned long flags = 0;
	int bkt;
	struct hlist_node *tmp;

	//To clean up SOS when uos got rebooted and stream did not
	//get closed properly. Current implementation only handle
	//for single UOS.
	spin_lock_irqsave(&stream_node_hash_lock, flags);
	if (!hash_empty(STREAM_NODE_HASH)) {
		hash_for_each_safe(STREAM_NODE_HASH, bkt, tmp, sn, node) {
			if (sn != NULL) {
				pr_debug("%s: performing stream clean up!",
								__func__);
				filp_close(sn->f, 0);
				hash_del(&sn->node);
				kfree(sn);
			}
		}
	}
	spin_unlock_irqrestore(&stream_node_hash_lock, flags);
}

static int process_device_open(struct ipu4_virtio_req_info *req_info)
{
	unsigned long flags = 0;
	char node_name[25];
	struct stream_node *sn = NULL;
	struct ici_stream_device *strm_dev;
	struct ipu4_virtio_req *req = req_info->request;
	int domid = req_info->domid;

	if (!hash_initialised) {
		hash_init(STREAM_NODE_HASH);
		hash_initialised = true;
		spin_lock_init(&stream_node_hash_lock);
	}

	spin_lock_irqsave(&stream_node_hash_lock, flags);
	hash_for_each_possible(STREAM_NODE_HASH, sn, node, req->op[0]) {
		if (sn != NULL) {
			if (sn->client_id != domid) {
				pr_err("%s: stream device %d already opened by other guest!",
					__func__, sn->client_id);
				spin_unlock_irqrestore(&stream_node_hash_lock,
													flags);
				return IPU4_REQ_ERROR;
			}
			pr_info("%s: stream device %d already opened by client %d",
				__func__, req->op[0], domid);
			spin_unlock_irqrestore(&stream_node_hash_lock,
												flags);
			return IPU4_REQ_ERROR;
		}
	}
	spin_unlock_irqrestore(&stream_node_hash_lock, flags);

	sprintf(node_name, "/dev/intel_stream%d", req->op[0]);
	pr_info("process_device_open: %s", node_name);
	sn = kzalloc(sizeof(struct stream_node), GFP_KERNEL);
	sn->f = filp_open(node_name, O_RDWR | O_NONBLOCK, 0);

	strm_dev = sn->f->private_data;
	if (strm_dev == NULL) {
		pr_err("Native IPU stream device not found\n");
		return IPU4_REQ_ERROR;
	}
	strm_dev->virt_dev_id = req->op[0];

	sn->client_id = domid;
	spin_lock_irqsave(&stream_node_hash_lock, flags);
	hash_add(STREAM_NODE_HASH, &sn->node, req->op[0]);
	spin_unlock_irqrestore(&stream_node_hash_lock, flags);

	return IPU4_REQ_PROCESSED;
}

static int process_device_close(struct ipu4_virtio_req_info *req_info)
{
	unsigned long flags = 0;
	struct stream_node *sn = NULL;
	struct hlist_node *tmp;
	struct ipu4_virtio_req *req = req_info->request;

	if (!hash_initialised)
		return IPU4_REQ_PROCESSED; //no node has been opened, do nothing

	pr_info("process_device_close: %d", req->op[0]);

	spin_lock_irqsave(&stream_node_hash_lock, flags);
	hash_for_each_possible_safe(STREAM_NODE_HASH, sn,
							tmp, node, req->op[0]) {
		if (sn != NULL) {
			filp_close(sn->f, 0);
			hash_del(&sn->node);
			kfree(sn);
		}
	}
	spin_unlock_irqrestore(&stream_node_hash_lock, flags);

	return IPU4_REQ_PROCESSED;
}

int process_set_format(struct ipu4_virtio_req_info *req_info)
{
	struct stream_node *sn = NULL;
	struct ici_stream_device *strm_dev;
	struct ici_stream_format *host_virt;
	int err, found;
	struct ipu4_virtio_req *req = req_info->request;
	int domid = req_info->domid;

	pr_debug("process_set_format: %d %d", hash_initialised, req->op[0]);

	if (!hash_initialised)
		return IPU4_REQ_ERROR;

	found = 0;
	hash_for_each_possible(STREAM_NODE_HASH, sn, node, req->op[0]) {
		if (sn != NULL) {
			pr_err("process_set_format: node %d %p", req->op[0], sn);
			found = 1;
			break;
		}
	}

	if (!found) {
		pr_debug("%s: stream not found %d\n", __func__, req->op[0]);
		return IPU4_REQ_ERROR;
	}

	strm_dev = sn->f->private_data;
	if (strm_dev == NULL) {
		pr_err("Native IPU stream device not found\n");
		return IPU4_REQ_ERROR;
	}

	host_virt = map_guest_phys(domid, req->payload,
						sizeof(struct ici_stream_format));
	if (host_virt == NULL) {
		pr_err("process_set_format: NULL host_virt");
		return IPU4_REQ_ERROR;
	}

	err = strm_dev->ipu_ioctl_ops->ici_set_format(sn->f, strm_dev, host_virt);

	unmap_guest_phys(domid, req->payload);

	if (err) {
		pr_err("intel_ipu4_pvirt: internal set fmt failed\n");
		return IPU4_REQ_ERROR;
	}
	else
		return IPU4_REQ_PROCESSED;
}

int process_poll(struct ipu4_virtio_req_info *req_info)
{
	struct stream_node *sn = NULL;
	struct ici_isys_stream *as;
	bool found, empty;
	unsigned long flags = 0;
	struct ipu4_virtio_req *req = req_info->request;
	int time_remain;

	pr_debug("%s: %d %d", __func__, hash_initialised, req->op[0]);

	if (!hash_initialised)
		return IPU4_REQ_ERROR;

	found = false;
	hash_for_each_possible(STREAM_NODE_HASH, sn, node, req->op[0]) {
		if (sn != NULL) {
			found = true;
			break;
		}
	}
	if (!found) {
		pr_debug("%s: stream not found %d\n", __func__, req->op[0]);
		return IPU4_REQ_ERROR;
	}

	as = dev_to_stream(sn->f->private_data);

	spin_lock_irqsave(&as->buf_list.lock, flags);
	empty = list_empty(&as->buf_list.putbuf_list);
	spin_unlock_irqrestore(&as->buf_list.lock, flags);
	if (!empty) {
		req->func_ret = 1;
		pr_debug("%s: done", __func__);
		return IPU4_REQ_PROCESSED;
	} else {
		time_remain = wait_event_interruptible_timeout(
			as->buf_list.wait,
			!list_empty(&as->buf_list.putbuf_list) ||
			!as->ip.streaming,
			POLL_WAIT);
		if((time_remain == -ERESTARTSYS) ||
			time_remain == 0 ||
			!as->ip.streaming) {
			pr_err("%s poll timeout or unexpected wake up! code:%d streaming: %d port:%d",
							__func__, time_remain,
							as->ip.streaming,
							req->op[0]);
			req->func_ret = 0;
			return IPU4_REQ_ERROR;
		}
		else {
			req->func_ret = POLLIN;
			return IPU4_REQ_PROCESSED;
		}
	}
}

int process_put_buf(struct ipu4_virtio_req_info *req_info)
{
	struct stream_node *sn = NULL;
	struct ici_stream_device *strm_dev;
	struct ici_frame_info *host_virt;
	int err, found;
	struct ipu4_virtio_req *req = req_info->request;
	int domid = req_info->domid;

	pr_debug("process_put_buf: %d %d", hash_initialised, req->op[0]);

	if (!hash_initialised)
		return IPU4_REQ_ERROR;

	found = 0;
	hash_for_each_possible(STREAM_NODE_HASH, sn, node, req->op[0]) {
		if (sn != NULL) {
			pr_debug("process_put_buf: node %d %p", req->op[0], sn);
			found = 1;
			break;
		}
	}

	if (!found) {
		pr_debug("%s: stream not found %d\n", __func__, req->op[0]);
		return IPU4_REQ_ERROR;
	}

	strm_dev = sn->f->private_data;
	if (strm_dev == NULL) {
		pr_err("Native IPU stream device not found\n");
		return IPU4_REQ_ERROR;
	}

	host_virt = map_guest_phys(domid, req->payload,
						sizeof(struct ici_frame_info));
	if (host_virt == NULL) {
		pr_err("process_put_buf: NULL host_virt");
		return IPU4_REQ_ERROR;
	}
	err = strm_dev->ipu_ioctl_ops->ici_put_buf(sn->f, strm_dev, host_virt);

	unmap_guest_phys(domid, req->payload);

	if (err) {
		pr_err("process_put_buf: ici_put_buf failed\n");
		return IPU4_REQ_ERROR;
	}
	else
		return IPU4_REQ_PROCESSED;
}

int process_get_buf(struct ipu4_virtio_req_info *req_info)
{
	struct stream_node *sn = NULL;
	struct ici_frame_buf_wrapper *shared_buf;
	struct ici_stream_device *strm_dev;
	int k, i = 0;
	void *pageaddr;
	u64 *page_table = NULL;
	struct page **data_pages = NULL;
	int err, found, status;
	struct ipu4_virtio_req *req = req_info->request;
	int domid = req_info->domid;

	pr_debug("process_get_buf: %d %d", hash_initialised, req->op[0]);

	if (!hash_initialised)
		return IPU4_REQ_ERROR;

	found = 0;
	hash_for_each_possible(STREAM_NODE_HASH, sn, node, req->op[0]) {
		if (sn != NULL) {
			pr_debug("process_get_buf: node %d %p", req->op[0], sn);
			found = 1;
			break;
		}
	}

	if (!found) {
		pr_debug("%s: stream not found %d\n", __func__, req->op[0]);
		return IPU4_REQ_ERROR;
	}

	pr_debug("GET_BUF: Mapping buffer\n");
	shared_buf = map_guest_phys(domid, req->payload,
					sizeof(struct ici_frame_buf_wrapper));
	if (!shared_buf) {
		pr_err("SOS Failed to map Buffer from UserOS\n");
		status = IPU4_REQ_ERROR;
		goto exit;
	}
	data_pages = kcalloc(shared_buf->kframe_info.planes[0].npages, sizeof(struct page *), GFP_KERNEL);
	if (data_pages == NULL) {
		pr_err("SOS Failed alloc data page set\n");
		status = IPU4_REQ_ERROR;
		goto exit_payload;
	}
	pr_debug("Total number of pages:%d\n", shared_buf->kframe_info.planes[0].npages);

	page_table = map_guest_phys(domid, shared_buf->kframe_info.planes[0].page_table_ref,
								shared_buf->kframe_info.planes[0].npages * sizeof(u64));

	if (page_table == NULL) {
		pr_err("SOS Failed to map page table\n");
		req->stat = IPU4_REQ_ERROR;
		status = IPU4_REQ_ERROR;
		goto exit_payload;
	}
	else {
		 pr_debug("SOS first page %lld\n", page_table[0]);
		 k = 0;
		 for (i = 0; i < shared_buf->kframe_info.planes[0].npages; i++) {
			 pageaddr = map_guest_phys(domid, page_table[i], PAGE_SIZE);
			 if (pageaddr == NULL) {
				 pr_err("Cannot map pages from UOS\n");
				 req->stat = IPU4_REQ_ERROR;
				 break;
			 }

			 data_pages[k] = virt_to_page(pageaddr);
			 k++;
		 }
	 }

	strm_dev = sn->f->private_data;
	if (strm_dev == NULL) {
		pr_err("Native IPU stream device not found\n");
		status = IPU4_REQ_ERROR;
		goto exit_page_table;
	}
	err = strm_dev->ipu_ioctl_ops->ici_get_buf_virt(sn->f, strm_dev, shared_buf, data_pages);

	if (err) {
		pr_err("process_get_buf: ici_get_buf_virt failed\n");
		status = IPU4_REQ_ERROR;
	}
	else
		status = IPU4_REQ_PROCESSED;

exit_page_table:
	for (i = 0; i < shared_buf->kframe_info.planes[0].npages; i++)
		unmap_guest_phys(domid, page_table[i]);
	unmap_guest_phys(domid, shared_buf->kframe_info.planes[0].page_table_ref);
exit_payload:
	kfree(data_pages);
	unmap_guest_phys(domid, req->payload);
exit:
	req->stat = status;
	return status;
}

int process_stream_on(struct ipu4_virtio_req_info *req_info)
{
	struct stream_node *sn = NULL;
	struct ici_stream_device *strm_dev;
	int err, found;
	struct ipu4_virtio_req *req = req_info->request;

	pr_debug("process_stream_on: %d %d", hash_initialised, req->op[0]);

	if (!hash_initialised)
		return IPU4_REQ_ERROR;

	found = 0;
	hash_for_each_possible(STREAM_NODE_HASH, sn, node, req->op[0]) {
		if (sn != NULL) {
			pr_err("process_stream_on: node %d %p", req->op[0], sn);
			found = 1;
			break;
		}
	}

	if (!found) {
		pr_debug("%s: stream not found %d\n", __func__, req->op[0]);
		return IPU4_REQ_ERROR;
	}

	strm_dev = sn->f->private_data;
	if (strm_dev == NULL) {
		pr_err("Native IPU stream device not found\n");
		return IPU4_REQ_ERROR;
	}

	err = strm_dev->ipu_ioctl_ops->ici_stream_on(sn->f, strm_dev);

	if (err) {
		pr_err("process_stream_on: stream on failed\n");
		return IPU4_REQ_ERROR;
	}
	else
		return IPU4_REQ_PROCESSED;
}

int process_stream_off(struct ipu4_virtio_req_info *req_info)
{
	struct stream_node *sn = NULL;
	struct ici_stream_device *strm_dev;
	struct ici_isys_stream *as;
	int err, found;
	struct ipu4_virtio_req *req = req_info->request;

	pr_debug("process_stream_off: %d %d", hash_initialised, req->op[0]);

	if (!hash_initialised)
		return IPU4_REQ_ERROR;

	found = 0;
	hash_for_each_possible(STREAM_NODE_HASH, sn, node, req->op[0]) {
		if (sn != NULL) {
			pr_err("process_stream_off: node %d %p", req->op[0], sn);
			found = 1;
			break;
		}
	}

	if (!found) {
		pr_debug("%s: stream not found %d\n", __func__, req->op[0]);
		return IPU4_REQ_ERROR;
	}

	strm_dev = sn->f->private_data;
	if (strm_dev == NULL) {
		pr_err("Native IPU stream device not found\n");
		return IPU4_REQ_ERROR;
	}

	err = strm_dev->ipu_ioctl_ops->ici_stream_off(sn->f, strm_dev);

	if (err) {
		pr_err("%s: stream off failed\n", __func__);
		return IPU4_REQ_ERROR;
	} else {
		as = dev_to_stream(strm_dev);
		wake_up_interruptible(&as->buf_list.wait);
		return IPU4_REQ_PROCESSED;
	}
}

int process_set_format_thread(void *data)
{
	int status;

	status = process_set_format(data);
	notify_fe(status, data);
	do_exit(0);
	return 0;
}

int process_device_open_thread(void *data)
{
	int status;

	status = process_device_open(data);
	notify_fe(status, data);
	do_exit(0);
	return 0;
}

int process_device_close_thread(void *data)
{
	int status;

	status = process_device_close(data);
	notify_fe(status, data);
	do_exit(0);
	return 0;
}

int process_poll_thread(void *data)
{
	int status;

	status = process_poll(data);
	notify_fe(status, data);
	do_exit(0);
	return 0;
}

int process_put_buf_thread(void *data)
{
	int status;

	status = process_put_buf(data);
	notify_fe(status, data);
	do_exit(0);
	return 0;
}

int process_stream_on_thread(void *data)
{
	int status;

	status = process_stream_on(data);
	notify_fe(status, data);
	do_exit(0);
	return 0;
}

int process_stream_off_thread(void *data)
{
	int status;

	status = process_stream_off(data);
	notify_fe(status, data);
	do_exit(0);
	return 0;
}

int process_get_buf_thread(void *data)
{
	int status;

	status = process_get_buf(data);
	notify_fe(status, data);
	do_exit(0);
	return 0;
}
