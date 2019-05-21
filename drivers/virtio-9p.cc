/*-
 * Copyright (c) 2015 iXsystems Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer
 *    in this position and unchanged.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
 * virtio driver for 9P
 */

#include <sys/cdefs.h>

#include "drivers/virtio.hh"
#include "drivers/pci-device.hh"
#include "drivers/virtio-9p.hh"
#include <osv/interrupt.hh>

#include <osv/mempool.hh>
#include <osv/mmu.hh>

#include <string>
#include <string.h>
#include <map>
#include <errno.h>
#include <osv/debug.h>

#include <osv/sched.hh>
#include "osv/trace.hh"

#include <osv/device.h>
#include <osv/bio.h>

TRACEPOINT(trace_virtio_9p_read_config_tag_len, "len=%d", int);
TRACEPOINT(trace_virtio_vt9p_read_config_mount_tag, "tag=%s", char *);
TRACEPOINT(trace_virtio_vt9p_wake, "");


using namespace memory;


namespace virtio {

int vt9p::_instance = 0;
std::vector<vt9p *> vt9p::_vt9p_drivers;
mutex vt9p::_drivers_lock;

bool vt9p::ack_irq()
{
    auto isr = virtio_conf_readb(VIRTIO_PCI_ISR);
    auto queue = get_virt_queue(0);

    if (isr) {
        queue->disable_interrupts();
        return true;
    } else {
        return false;
    }

}

vt9p::vt9p(pci::device& pci_dev)
    : virtio_driver(pci_dev), _client(nullptr)
{

    _driver_name = "virtio-9p";
    _id = _instance++;
    virtio_i("VIRTIO 9P INSTANCE %d", _id);

    // Steps 4, 5 & 6 - negotiate and confirm features
    setup_features();
    read_config();

    //register the single irq callback for the block
    sched::thread* t = sched::thread::make([this] { this->req_done(); },
            sched::thread::attr().name("virtio-9p"));
    t->start();
    auto queue = get_virt_queue(0);
    if (pci_dev.is_msix()) {
        _msi.easy_register({ { 0, [=] { queue->disable_interrupts(); }, t } });
    } else {
        _irq.reset(new pci_interrupt(pci_dev,
                                     [=] { return ack_irq(); },
                                     [=] { t->wake(); }));
    }

    // Enable indirect descriptor
    queue->set_use_indirect(true);

    // Step 8
    add_dev_status(VIRTIO_CONFIG_S_DRIVER_OK);

    debugf("virtio-9p: Add 9p driver instances %d\n", _id);

    // Register into _vt9p_drivers
    _vt9p_drivers.push_back(this);
}

vt9p::~vt9p()
{
    //TODO: In theory maintain the list of free instances and gc it
    // including the thread objects and their stack
    // Unregister from _vt9p_drivers
    for (auto it = _vt9p_drivers.begin(); it != _vt9p_drivers.end(); it++)
    {
        if ((*it)->_id == _id)
        {
            _vt9p_drivers.erase(it);
            break;
        }
    }
    if (_config.tag)
    {
        free(_config.tag);
    }
}

/* Read 9P Config :only mount tag
 */
void vt9p::read_config()
{
    //read all of the block config (including size, mce, topology,..) in one shot
    virtio_conf_read(virtio_pci_config_offset(), &_config, sizeof(_config));
    _config.tag = (char *) malloc(_config.tag_len);
    virtio_conf_read(virtio_pci_config_offset() + sizeof(_config.tag_len), 
        _config.tag, _config.tag_len);

    trace_virtio_9p_read_config_tag_len(_config.tag_len);

    if (get_guest_feature_bit(VIRTIO_9P_F_MOUNT_TAG))
        trace_virtio_vt9p_read_config_mount_tag(_config.tag);
}

/* Deal requests in a separate thread
 */
void vt9p::req_done()
{
    auto* queue = get_virt_queue(0);
    struct p9_req_t *req;

    while (1) {

        virtio_driver::wait_for_queue(queue, &vring::used_ring_not_empty);
        trace_virtio_vt9p_wake();

        u32 len;
        if((req = static_cast<struct p9_req_t *>(queue->get_buf_elem(&len))) != nullptr) {
            if (len)
                p9_client::p9_client_cb(req, REQ_STATUS_RCVD);
            queue->get_buf_finalize();
            // wake up the requesting thread in case the ring was full before
            queue->wakeup_waiter();
        }
        
    }
}


static const int sector_size = 512;

int vt9p::make_request(struct p9_req_t *req)
{
    // The lock is here for parallel requests protection
    WITH_LOCK(_lock) {

        if (!req)
            return EIO;

        auto* queue = get_virt_queue(0);

        req->status = REQ_STATUS_SENT;

        queue->init_sg();
        if (req->tc->size)
            queue->add_out_sg(req->tc->sdata, req->tc->size);
        if (req->rc->capacity)
            queue->add_in_sg(req->rc->sdata, req->rc->capacity);

        queue->add_buf_wait(req);

        queue->kick();

        return 0;
    }
}

/* Get 9P Feature bits:only VIRTIO_9P_F_MOUNT_TAG
 */
u32 vt9p::get_driver_features()
{
    auto base = virtio_driver::get_driver_features();
    return (base | ( 1 << VIRTIO_9P_F_MOUNT_TAG));
}

hw_driver* vt9p::probe(hw_device* dev)
{
    return virtio::probe<vt9p, VIRTIO_9P_DEVICE_ID>(dev);
}

int vt9p::bind_client(struct p9_client *client, const char *devname, char *args)
{
    int ret = -EBUSY;
    WITH_LOCK(_drivers_lock)
    {
        for (auto it = _vt9p_drivers.begin(); it != _vt9p_drivers.end(); it++)
        {
            if (!strncmp(devname, (*it)->_config.tag, (*it)->_config.tag_len) && 
            strlen(devname) == (*it)->_config.tag_len)
            {
                if (!(*it)->_client)
                {
                    (*it)->_client = client;
                    client->p9_client_connect((*it));
                    ret = 0;
                    break;
                }
            }
        }
    }

    return ret;
}

int vt9p::unbind_client(struct p9_client *client)
{
    vt9p *vt = (vt9p *) client->p9_trans();

    WITH_LOCK(_drivers_lock)
    {
        if (vt)
        {
            vt->_client = nullptr;
            return 0;
        }
    }

    return -ENOENT;
}

}