/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 Isospin Authors
 * SPDX-License-Identifier: MIT
 *
 * nvidia-shim.ko - GPU broker shim for guest VMs
 *
 * This module intercepts NVIDIA ioctls and forwards them to the GPU broker
 * on the host via vsock. It allows unmodified CUDA applications to run in
 * VMs while the actual GPU operations are proxied.
 *
 * Architecture:
 *   CUDA app → /dev/nvidiactl → this shim → vsock → broker → real GPU
 *
 * Usage:
 *   insmod nvidia-shim.ko broker_cid=2 broker_port=9999
 *
 * The broker_cid is typically 2 (VMADDR_CID_HOST) to connect to the host.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/poll.h>
#include <linux/mm.h>
#include <linux/wait.h>
#include <linux/sched.h>
#include <linux/atomic.h>
#include <linux/mutex.h>
#include <linux/kthread.h>
#include <linux/net.h>
#include <linux/socket.h>
#include <linux/vm_sockets.h>

#define NV_SHIM_NAME        "nvidia-shim"
#define NV_SHIM_VERSION     "0.2.0"

/* NVIDIA uses major 195 */
#define NV_MAJOR            195
#define NV_CTL_MINOR        255
#define NV_MAX_DEVICES      32

/* Maximum ioctl data size */
#define MAX_IOCTL_SIZE      4096

/* Timeout for broker responses (ms) */
#define RESPONSE_TIMEOUT_MS 5000

/* Wire protocol constants */
#define WIRE_MAGIC          0x4E56424B  /* "NVBK" */
#define WIRE_VERSION        1

/* Operation types - must match broker */
#define OP_REGISTER_CLIENT  0
#define OP_UNREGISTER       1
#define OP_ALLOC            2
#define OP_FREE             3
#define OP_CONTROL          4
#define OP_MAP_MEMORY       5
#define OP_UNMAP_MEMORY     6
#define OP_CARD_INFO        7
#define OP_CHECK_VERSION    8
#define OP_REGISTER_FD      9
#define OP_ALLOC_OS_EVENT   10
#define OP_FREE_OS_EVENT    11
#define OP_ATTACH_GPUS      12
#define OP_STATUS_CODE      13
#define OP_DUP_OBJECT       14
#define OP_SHARE            15

/* NVIDIA escape code ranges */
#define NV_ESC_CARD_INFO         200
#define NV_ESC_REGISTER_FD       201
#define NV_ESC_ALLOC_OS_EVENT    206
#define NV_ESC_FREE_OS_EVENT     207
#define NV_ESC_STATUS_CODE       209
#define NV_ESC_CHECK_VERSION_STR 210
#define NV_ESC_ATTACH_GPUS_TO_FD 212

/* RM escape codes */
#define NV_ESC_RM_ALLOC_MEMORY   0x27
#define NV_ESC_RM_ALLOC_OBJECT   0x28
#define NV_ESC_RM_FREE           0x29
#define NV_ESC_RM_CONTROL        0x2A
#define NV_ESC_RM_ALLOC          0x2B
#define NV_ESC_RM_DUP_OBJECT     0x34
#define NV_ESC_RM_SHARE          0x35
#define NV_ESC_RM_MAP_MEMORY     0x4E
#define NV_ESC_RM_UNMAP_MEMORY   0x4F

/* NVIDIA ioctl magic */
#define NV_IOCTL_MAGIC      'F'

MODULE_LICENSE("MIT");
MODULE_AUTHOR("Isospin Authors");
MODULE_DESCRIPTION("NVIDIA GPU broker shim for guest VMs (vsock transport)");
MODULE_VERSION(NV_SHIM_VERSION);

/* --------------------------------------------------------------------------
 * Wire Protocol Structures
 * -------------------------------------------------------------------------- */

/*
 * Request header sent over vsock
 */
struct wire_request {
    u32 magic;
    u32 version;
    u64 client_id;
    u64 seq;
    u32 op_type;
    u32 payload_len;
    /* payload follows */
} __attribute__((packed));

/*
 * Response header received over vsock
 */
struct wire_response {
    u32 magic;
    u32 version;
    u64 client_id;
    u64 seq;
    u8  success;
    u8  _pad[3];
    u32 result_len;
    /* result follows */
} __attribute__((packed));

/* --------------------------------------------------------------------------
 * Data Structures
 * -------------------------------------------------------------------------- */

/*
 * Broker connection state
 */
struct nv_shim_broker {
    struct socket *sock;
    struct mutex lock;
    u64 client_id;
    atomic64_t next_seq;
    bool connected;
    
    /* Reconnection */
    struct task_struct *connect_thread;
    wait_queue_head_t connect_wq;
    bool should_stop;
};

/*
 * Per-open-file state
 */
struct nv_shim_file {
    struct nv_shim_broker *broker;
    int minor;
};

/*
 * Global driver state
 */
static struct {
    struct class *class;
    struct cdev cdev;
    dev_t devno;
    struct nv_shim_broker *broker;
} nv_shim;

/* Module parameters */
static unsigned int broker_cid = 2;  /* VMADDR_CID_HOST */
static unsigned int broker_port = 9999;

module_param(broker_cid, uint, 0444);
MODULE_PARM_DESC(broker_cid, "Broker vsock CID (default: 2 = host)");

module_param(broker_port, uint, 0444);
MODULE_PARM_DESC(broker_port, "Broker vsock port (default: 9999)");

/* --------------------------------------------------------------------------
 * vsock Communication
 * -------------------------------------------------------------------------- */

/*
 * Connect to the broker over vsock
 */
static int
nv_shim_connect(struct nv_shim_broker *broker)
{
    struct sockaddr_vm addr;
    int ret;
    
    if (broker->connected)
        return 0;
    
    mutex_lock(&broker->lock);
    
    if (broker->connected) {
        mutex_unlock(&broker->lock);
        return 0;
    }
    
    /* Create vsock socket */
    ret = sock_create_kern(&init_net, AF_VSOCK, SOCK_STREAM, 0, &broker->sock);
    if (ret < 0) {
        pr_err("nv-shim: failed to create vsock: %d\n", ret);
        mutex_unlock(&broker->lock);
        return ret;
    }
    
    /* Connect to broker */
    memset(&addr, 0, sizeof(addr));
    addr.svm_family = AF_VSOCK;
    addr.svm_cid = broker_cid;
    addr.svm_port = broker_port;
    
    ret = kernel_connect(broker->sock, (struct sockaddr *)&addr,
                         sizeof(addr), 0);
    if (ret < 0) {
        pr_err("nv-shim: failed to connect to broker at %u:%u: %d\n",
               broker_cid, broker_port, ret);
        sock_release(broker->sock);
        broker->sock = NULL;
        mutex_unlock(&broker->lock);
        return ret;
    }
    
    pr_info("nv-shim: connected to broker at cid=%u port=%u\n",
            broker_cid, broker_port);
    
    broker->connected = true;
    mutex_unlock(&broker->lock);
    
    return 0;
}

/*
 * Disconnect from broker
 */
static void
nv_shim_disconnect(struct nv_shim_broker *broker)
{
    mutex_lock(&broker->lock);
    
    if (broker->sock) {
        kernel_sock_shutdown(broker->sock, SHUT_RDWR);
        sock_release(broker->sock);
        broker->sock = NULL;
    }
    broker->connected = false;
    
    mutex_unlock(&broker->lock);
}

/*
 * Send data over vsock
 */
static int
nv_shim_send(struct nv_shim_broker *broker, void *data, size_t len)
{
    struct kvec iov = { .iov_base = data, .iov_len = len };
    struct msghdr msg = { };
    int ret;
    
    if (!broker->connected || !broker->sock)
        return -ENOTCONN;
    
    ret = kernel_sendmsg(broker->sock, &msg, &iov, 1, len);
    if (ret < 0) {
        pr_err("nv-shim: send failed: %d\n", ret);
        return ret;
    }
    if (ret != len) {
        pr_err("nv-shim: short send: %d < %zu\n", ret, len);
        return -EIO;
    }
    
    return 0;
}

/*
 * Receive data over vsock
 */
static int
nv_shim_recv(struct nv_shim_broker *broker, void *buf, size_t len)
{
    struct kvec iov = { .iov_base = buf, .iov_len = len };
    struct msghdr msg = { };
    size_t received = 0;
    int ret;
    
    if (!broker->connected || !broker->sock)
        return -ENOTCONN;
    
    while (received < len) {
        iov.iov_base = buf + received;
        iov.iov_len = len - received;
        
        ret = kernel_recvmsg(broker->sock, &msg, &iov, 1,
                             len - received, 0);
        if (ret < 0) {
            pr_err("nv-shim: recv failed: %d\n", ret);
            return ret;
        }
        if (ret == 0) {
            pr_err("nv-shim: connection closed by broker\n");
            broker->connected = false;
            return -ECONNRESET;
        }
        received += ret;
    }
    
    return 0;
}

/*
 * Send a request and receive response
 */
static int
nv_shim_rpc(struct nv_shim_broker *broker,
            u32 op_type,
            void *payload, size_t payload_len,
            void *result, size_t result_size, size_t *result_len)
{
    struct wire_request *req;
    struct wire_response resp;
    size_t req_size;
    u64 seq;
    int ret;
    
    req_size = sizeof(*req) + payload_len;
    req = kzalloc(req_size, GFP_KERNEL);
    if (!req)
        return -ENOMEM;
    
    /* Build request */
    seq = atomic64_fetch_add(1, &broker->next_seq);
    req->magic = WIRE_MAGIC;
    req->version = WIRE_VERSION;
    req->client_id = broker->client_id;
    req->seq = seq;
    req->op_type = op_type;
    req->payload_len = payload_len;
    
    if (payload_len > 0 && payload)
        memcpy(req + 1, payload, payload_len);
    
    mutex_lock(&broker->lock);
    
    /* Send request */
    ret = nv_shim_send(broker, req, req_size);
    if (ret < 0)
        goto out;
    
    /* Receive response header */
    ret = nv_shim_recv(broker, &resp, sizeof(resp));
    if (ret < 0)
        goto out;
    
    /* Validate response */
    if (resp.magic != WIRE_MAGIC) {
        pr_err("nv-shim: bad response magic: 0x%x\n", resp.magic);
        ret = -EPROTO;
        goto out;
    }
    
    if (resp.seq != seq) {
        pr_err("nv-shim: seq mismatch: got %llu, expected %llu\n",
               resp.seq, seq);
        ret = -EPROTO;
        goto out;
    }
    
    /* Receive result payload */
    if (resp.result_len > 0) {
        if (resp.result_len > result_size) {
            pr_err("nv-shim: result too large: %u > %zu\n",
                   resp.result_len, result_size);
            ret = -ENOSPC;
            goto out;
        }
        
        ret = nv_shim_recv(broker, result, resp.result_len);
        if (ret < 0)
            goto out;
    }
    
    if (result_len)
        *result_len = resp.result_len;
    
    ret = resp.success ? 0 : -EIO;
    
out:
    mutex_unlock(&broker->lock);
    kfree(req);
    return ret;
}

/* --------------------------------------------------------------------------
 * ioctl Forwarding
 * -------------------------------------------------------------------------- */

/*
 * Determine operation type from escape code
 */
static u32
escape_to_op_type(u32 escape_code)
{
    switch (escape_code) {
    /* Top-level escapes (200+) */
    case NV_ESC_CARD_INFO:
        return OP_CARD_INFO;
    case NV_ESC_CHECK_VERSION_STR:
        return OP_CHECK_VERSION;
    case NV_ESC_REGISTER_FD:
        return OP_REGISTER_FD;
    case NV_ESC_ALLOC_OS_EVENT:
        return OP_ALLOC_OS_EVENT;
    case NV_ESC_FREE_OS_EVENT:
        return OP_FREE_OS_EVENT;
    case NV_ESC_ATTACH_GPUS_TO_FD:
        return OP_ATTACH_GPUS;
    case NV_ESC_STATUS_CODE:
        return OP_STATUS_CODE;
    
    /* RM escapes */
    case NV_ESC_RM_ALLOC:
    case NV_ESC_RM_ALLOC_MEMORY:
    case NV_ESC_RM_ALLOC_OBJECT:
        return OP_ALLOC;
    case NV_ESC_RM_FREE:
        return OP_FREE;
    case NV_ESC_RM_CONTROL:
        return OP_CONTROL;
    case NV_ESC_RM_DUP_OBJECT:
        return OP_DUP_OBJECT;
    case NV_ESC_RM_SHARE:
        return OP_SHARE;
    case NV_ESC_RM_MAP_MEMORY:
        return OP_MAP_MEMORY;
    case NV_ESC_RM_UNMAP_MEMORY:
        return OP_UNMAP_MEMORY;
    
    /* Everything else is a Control operation */
    default:
        return OP_CONTROL;
    }
}

/*
 * Build payload for specific operation types
 */
static int
build_payload(u32 op_type, u32 escape_code,
              void __user *arg, size_t arg_size,
              void *payload, size_t *payload_len)
{
    u8 *p = payload;
    
    switch (op_type) {
    case OP_CARD_INFO:
        /* No payload */
        *payload_len = 0;
        return 0;
        
    case OP_CHECK_VERSION:
        /* [cmd: u32] [reply: u32] [version_string...] */
        if (arg_size >= 8 && arg) {
            if (copy_from_user(p, arg, arg_size))
                return -EFAULT;
            *payload_len = arg_size;
        } else {
            *payload_len = 0;
        }
        return 0;
        
    case OP_ALLOC:
        /* [h_root: u32] [h_parent: u32] [h_object: u32] [class: u32] [params...] */
        if (arg_size > 0 && arg) {
            if (copy_from_user(p, arg, arg_size))
                return -EFAULT;
            *payload_len = arg_size;
        } else {
            *payload_len = 0;
        }
        return 0;
        
    case OP_FREE:
        /* [h_root: u32] [h_object: u32] */
        if (arg_size >= 8 && arg) {
            if (copy_from_user(p, arg, 8))
                return -EFAULT;
            *payload_len = 8;
        } else {
            *payload_len = 0;
        }
        return 0;
        
    case OP_CONTROL:
        /* [h_client: u32] [h_object: u32] [cmd: u32] [params_size: u32] [params...] */
        {
            u32 header[4];
            if (arg_size < 16) {
                *payload_len = 0;
                return 0;
            }
            if (copy_from_user(header, arg, 16))
                return -EFAULT;
            
            /* header[0] = hClient, header[1] = hObject, header[2] = cmd, header[3] = paramsSize */
            memcpy(p, header, 16);
            *payload_len = 16;
            
            /* Copy params if present */
            if (header[3] > 0 && arg_size > 24) {
                size_t params_size = min_t(size_t, header[3], arg_size - 24);
                if (copy_from_user(p + 16, (char __user *)arg + 24, params_size))
                    return -EFAULT;
                *payload_len += params_size;
            }
        }
        return 0;
        
    default:
        /* Generic: just copy raw params */
        if (arg_size > 0 && arg) {
            if (copy_from_user(p, arg, arg_size))
                return -EFAULT;
            *payload_len = arg_size;
        } else {
            *payload_len = 0;
        }
        return 0;
    }
}

/*
 * Forward an ioctl to the broker
 */
static int
nv_shim_forward_ioctl(struct nv_shim_broker *broker,
                      unsigned int cmd,
                      void __user *arg,
                      size_t arg_size)
{
    void *payload = NULL;
    void *result = NULL;
    size_t payload_len, result_len;
    u32 escape_code, op_type;
    int ret;
    
    /* Extract escape code from ioctl command */
    escape_code = _IOC_NR(cmd);
    op_type = escape_to_op_type(escape_code);
    
    pr_debug("nv-shim: ioctl escape=0x%x op=%u size=%zu\n",
             escape_code, op_type, arg_size);
    
    /* Allocate buffers */
    payload = kzalloc(MAX_IOCTL_SIZE, GFP_KERNEL);
    result = kzalloc(MAX_IOCTL_SIZE, GFP_KERNEL);
    if (!payload || !result) {
        ret = -ENOMEM;
        goto out;
    }
    
    /* Build payload */
    ret = build_payload(op_type, escape_code, arg, arg_size,
                        payload, &payload_len);
    if (ret < 0)
        goto out;
    
    /* Ensure connected */
    ret = nv_shim_connect(broker);
    if (ret < 0)
        goto out;
    
    /* Send RPC */
    ret = nv_shim_rpc(broker, op_type,
                      payload, payload_len,
                      result, MAX_IOCTL_SIZE, &result_len);
    if (ret < 0)
        goto out;
    
    /* Copy result back to user */
    if (result_len > 0 && arg_size > 0 && arg) {
        /*
         * Result format depends on op_type:
         * - CONTROL: [status: u32] [params_out...]
         * - ALLOC: [status: u32] [h_object: u32]
         * - Others: raw data
         */
        size_t copy_len = min(result_len, arg_size);
        if (copy_to_user(arg, result, copy_len)) {
            ret = -EFAULT;
            goto out;
        }
    }
    
    ret = 0;
    
out:
    kfree(payload);
    kfree(result);
    return ret;
}

/* --------------------------------------------------------------------------
 * File Operations
 * -------------------------------------------------------------------------- */

static int
nvidia_shim_open(struct inode *inode, struct file *file)
{
    struct nv_shim_file *nvf;
    int minor = iminor(inode);
    
    pr_debug("nv-shim: open minor=%d\n", minor);
    
    nvf = kzalloc(sizeof(*nvf), GFP_KERNEL);
    if (!nvf)
        return -ENOMEM;
    
    nvf->broker = nv_shim.broker;
    nvf->minor = minor;
    
    file->private_data = nvf;
    
    return 0;
}

static int
nvidia_shim_release(struct inode *inode, struct file *file)
{
    struct nv_shim_file *nvf = file->private_data;
    
    pr_debug("nv-shim: release minor=%d\n", nvf->minor);
    
    kfree(nvf);
    file->private_data = NULL;
    
    return 0;
}

static long
nvidia_shim_unlocked_ioctl(struct file *file,
                           unsigned int cmd,
                           unsigned long arg)
{
    struct nv_shim_file *nvf = file->private_data;
    size_t arg_size = _IOC_SIZE(cmd);
    u8 magic = _IOC_TYPE(cmd);
    
    /* Only handle NVIDIA ioctls */
    if (magic != NV_IOCTL_MAGIC) {
        pr_debug("nv-shim: ignoring non-nvidia ioctl magic=0x%x\n", magic);
        return -ENOTTY;
    }
    
    if (!nvf || !nvf->broker) {
        pr_err("nv-shim: ioctl on invalid file\n");
        return -EINVAL;
    }
    
    return nv_shim_forward_ioctl(nvf->broker, cmd,
                                 (void __user *)arg, arg_size);
}

static int
nvidia_shim_mmap(struct file *file, struct vm_area_struct *vma)
{
    pr_debug("nv-shim: mmap offset=0x%lx size=0x%lx\n",
             vma->vm_pgoff << PAGE_SHIFT,
             vma->vm_end - vma->vm_start);
    
    /*
     * GPU memory mapping requires coordination with broker.
     * For now, return error - data path would use different mechanism.
     */
    pr_warn("nv-shim: mmap not implemented (control path only)\n");
    return -ENOSYS;
}

static struct file_operations nvidia_shim_fops = {
    .owner          = THIS_MODULE,
    .open           = nvidia_shim_open,
    .release        = nvidia_shim_release,
    .unlocked_ioctl = nvidia_shim_unlocked_ioctl,
#ifdef CONFIG_COMPAT
    .compat_ioctl   = nvidia_shim_unlocked_ioctl,
#endif
    .mmap           = nvidia_shim_mmap,
};

/* --------------------------------------------------------------------------
 * Module Init/Exit
 * -------------------------------------------------------------------------- */

static int
nv_shim_create_broker(void)
{
    struct nv_shim_broker *broker;
    
    broker = kzalloc(sizeof(*broker), GFP_KERNEL);
    if (!broker)
        return -ENOMEM;
    
    mutex_init(&broker->lock);
    atomic64_set(&broker->next_seq, 1);
    broker->client_id = 1;  /* Will be assigned by broker on connect */
    broker->connected = false;
    broker->sock = NULL;
    
    nv_shim.broker = broker;
    
    /* Try initial connection (non-fatal if fails) */
    if (nv_shim_connect(broker) < 0) {
        pr_warn("nv-shim: initial connection failed, will retry on first ioctl\n");
    }
    
    return 0;
}

static void
nv_shim_destroy_broker(void)
{
    struct nv_shim_broker *broker = nv_shim.broker;
    
    if (!broker)
        return;
    
    nv_shim_disconnect(broker);
    mutex_destroy(&broker->lock);
    kfree(broker);
    nv_shim.broker = NULL;
}

static int __init
nvidia_shim_init(void)
{
    int ret;
    
    pr_info("nv-shim: " NV_SHIM_NAME " v" NV_SHIM_VERSION " loading\n");
    pr_info("nv-shim: broker_cid=%u broker_port=%u\n", broker_cid, broker_port);
    
    /* Allocate device numbers (major 195, minors 0-255) */
    nv_shim.devno = MKDEV(NV_MAJOR, 0);
    ret = register_chrdev_region(nv_shim.devno, NV_MAX_DEVICES + 1, NV_SHIM_NAME);
    if (ret < 0) {
        pr_err("nv-shim: failed to register chrdev region: %d\n", ret);
        pr_err("nv-shim: (is another nvidia driver loaded?)\n");
        return ret;
    }
    
    /* Initialize cdev */
    cdev_init(&nv_shim.cdev, &nvidia_shim_fops);
    nv_shim.cdev.owner = THIS_MODULE;
    
    ret = cdev_add(&nv_shim.cdev, nv_shim.devno, NV_MAX_DEVICES + 1);
    if (ret < 0) {
        pr_err("nv-shim: failed to add cdev: %d\n", ret);
        goto err_unregister;
    }
    
    /* Create device class */
    nv_shim.class = class_create(NV_SHIM_NAME);
    if (IS_ERR(nv_shim.class)) {
        ret = PTR_ERR(nv_shim.class);
        pr_err("nv-shim: failed to create class: %d\n", ret);
        goto err_cdev;
    }
    
    /* Create device nodes */
    device_create(nv_shim.class, NULL, MKDEV(NV_MAJOR, NV_CTL_MINOR),
                  NULL, "nvidiactl");
    device_create(nv_shim.class, NULL, MKDEV(NV_MAJOR, 0),
                  NULL, "nvidia0");
    
    /* Create broker connection */
    ret = nv_shim_create_broker();
    if (ret < 0) {
        pr_err("nv-shim: failed to create broker: %d\n", ret);
        goto err_devices;
    }
    
    pr_info("nv-shim: loaded successfully\n");
    pr_info("nv-shim: created /dev/nvidiactl and /dev/nvidia0\n");
    return 0;

err_devices:
    device_destroy(nv_shim.class, MKDEV(NV_MAJOR, 0));
    device_destroy(nv_shim.class, MKDEV(NV_MAJOR, NV_CTL_MINOR));
    class_destroy(nv_shim.class);
err_cdev:
    cdev_del(&nv_shim.cdev);
err_unregister:
    unregister_chrdev_region(nv_shim.devno, NV_MAX_DEVICES + 1);
    return ret;
}

static void __exit
nvidia_shim_exit(void)
{
    pr_info("nv-shim: unloading\n");
    
    nv_shim_destroy_broker();
    
    device_destroy(nv_shim.class, MKDEV(NV_MAJOR, 0));
    device_destroy(nv_shim.class, MKDEV(NV_MAJOR, NV_CTL_MINOR));
    class_destroy(nv_shim.class);
    cdev_del(&nv_shim.cdev);
    unregister_chrdev_region(nv_shim.devno, NV_MAX_DEVICES + 1);
    
    pr_info("nv-shim: unloaded\n");
}

module_init(nvidia_shim_init);
module_exit(nvidia_shim_exit);
