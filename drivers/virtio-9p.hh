
/*
 * virtio driver for 9P
 */

#ifndef VIRTIO_9P_DRIVER_H
#define VIRTIO_9P_DRIVER_H
#include "drivers/virtio.hh"
#include "drivers/pci-device.hh"
#include <osv/bio.h>
#include <osv/9pclient.hh>

namespace virtio {

class vt9p : public virtio_driver {
public:

    // The feature bitmap for virtio 9p
    enum {
        VIRTIO_9P_F_MOUNT_TAG  = 0,  /* The mount point is specified in config */
    };

    enum {
        VIRTIO_9P_DEVICE_ID = 0x1009,
        VIRTIO_9P_ID_BYTES = 20, /* ID string length */

        /*
         * Command types
         *
         * Usage is a bit tricky as some bits are used as flags and some are not.
         *
         * Rules:
         *   VIRTIO_BLK_T_OUT may be combined with VIRTIO_BLK_T_SCSI_CMD or
         *   VIRTIO_BLK_T_BARRIER.  VIRTIO_BLK_T_FLUSH is a command of its own
         *   and may not be combined with any of the other flags.
         */
    };

    enum vt9p_request_type {
        VIRTIO_9P_T_IN = 0,
        VIRTIO_9P_T_OUT = 1,
        /* This bit says it's a scsi command, not an actual read or write. */
        VIRTIO_9P_T_SCSI_CMD = 2,
        /* Cache flush command */
        VIRTIO_9P_T_FLUSH = 4,
        /* Get device ID command */
        VIRTIO_9P_T_GET_ID = 8,
        /* Barrier before this op. */
        VIRTIO_9P_T_BARRIER = 0x80000000,
    };

    enum vt9p_res_code {
        /* And this is the final byte of the write scatter-gather list. */
        VIRTIO_9P_S_OK = 0,
        VIRTIO_9P_S_IOERR = 1,
        VIRTIO_9P_S_UNSUPP = 2,
    };

    struct vt9p_config {
            uint16_t tag_len;
            char *tag;
    } __attribute__((packed));


    struct vt9p_res {
        u8 status;
    };

    explicit vt9p(pci::device& dev);
    virtual ~vt9p();

    virtual std::string get_name() const { return _driver_name; }
    void read_config();

    virtual u32 get_driver_features();

    int make_request(struct p9_req_t *req);

    void req_done();

    bool ack_irq();

    static hw_driver* probe(hw_device* dev);

    static int bind_client(struct p9_client *client, const char *devname, char *args);

    static int unbind_client(struct p9_client *client);

private:

    std::string _driver_name;

    uint64_t _cfg;
    uint64_t _features;
    vt9p_config _config;

    struct p9_client *_client;

    // Maintain all vt9p instances for reusing
    static std::vector<vt9p *> _vt9p_drivers;
    // This mutext proects _vt9p_drivers
    static mutex _drivers_lock;


    //maintains the virtio instance number for multiple drives
    static int _instance;
    int _id;
    // This mutex protects parallel make_request invocations
    mutex _lock;
    std::unique_ptr<pci_interrupt> _irq;
};

}
#endif