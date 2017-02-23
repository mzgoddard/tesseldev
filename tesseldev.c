#include <linux/module.h>
#include <linux/version.h>
#include <linux/kmod.h>

#include <linux/kernel.h>
#include <linux/spinlock.h>
#include <linux/kfifo.h>
#include <linux/wait.h>
#include <linux/completion.h>
#include <linux/poll.h>
#include <linux/fs.h>
#include <linux/spi/spi.h>
#include <linux/interrupt.h>
#include <linux/workqueue.h>
#include <linux/gpio.h>
#include <linux/delay.h>
#include <linux/socket.h>
#include <linux/net.h>
#include <net/af_unix.h>
#include <linux/syscalls.h>

#define DEVICE_NAME "tesseldev"
#define CLASS_NAME "tessel"

#define RING_FIFO_SIZE 4096
#define BUFSIZE 255

#define STATUS_TRUE 1
#define STATUS_FALSE 0
#define STATUS_BYTE 0x01
#define STATUS_BIT 0x10

#define N_CHANNEL 3
#define USBD_CHANNEL 0
#define CONN_USB 0
#define CONN_PORT_A 1
#define CONN_PORT_B 2
#define SOCKET_DIR "/var/run/tessel"

int pin_irq = 2;
int pin_sync = 1;

#define debug(args...)
// #define debug(args...)  printk(KERN_DEBUG "TesselDev: " args)
#define info(args...)   printk(KERN_INFO "TesselDev: " args)
#define error(args...)  printk(KERN_ERR "TesselDev: " args)
#define fatal(args...) ({ \
    printk(KERN_CRIT "TesselDev: " args); \
})

struct ring_file {
    struct kfifo fifo;
    spinlock_t lock;

    // If read, waiting for data from MCU. If write, waiting for space to write
    // to MCU.
    struct completion wait_data;
};

struct ring_device {
    struct ring_file input;
    struct ring_file output;

    // size_t spi_put_len;
    // size_t spi_get_len;

    struct socket *socket;
    struct work_struct socket_state_prework;
    struct work_struct socket_state_work;
    struct work_struct socket_read_prework;
    struct work_struct socket_write_prework;
    struct work_struct socket_read_work;
    struct {
        char body[BUFSIZE];
        size_t len;
        struct kvec vec;
    } socket_read;
    struct work_struct socket_write_work;
    struct work_struct socket_cleanup_work;

    unsigned int minor;
};

static ssize_t ring_read(struct file *, char __user *, size_t, loff_t *);
static ssize_t ring_write(struct file *, const char __user *, size_t, loff_t *);
static int ring_open(struct inode *, struct file *);
// static int ring_poll(struct file *, struct poll_table_struct *);
static int ring_release(struct inode *, struct file *);

// static void __ring_spi_add(
//     struct ring_device *, struct spi_message *, struct spi_transfer **,
//     size_t, void *, void *);
// static size_t tesselring_spi_put_prepare(
//     struct ring_device *, struct spi_message *, struct spi_transfer *,
//     size_t);
// static void ring_spi_put_finish(struct ring_device *);
// static size_t ring_spi_get_prepare(
//     struct ring_device *, struct spi_message *message, struct spi_transfer *,
//     size_t);
// static void ring_spi_get_finish(struct ring_device *);

static void ring_socket_noop(struct sock *sk);
static void ring_state_change(struct sock *sk);
static void ring_data_ready(struct sock *sk);
static void ring_write_space(struct sock *sk);
static void ring_socket_prestate(struct work_struct *work);
static void ring_socket_state(struct work_struct *work);
static void ring_socket_preread(struct work_struct *work);
static void ring_socket_prewrite(struct work_struct *work);
static void ring_socket_read(struct work_struct *work);
static void ring_socket_write(struct work_struct *work);
static void ring_socket_cleanup(struct work_struct *work);

static uint8_t get_channel_bitmask_state(uint8_t *bitmask, uint8_t channel);
static void set_channel_bitmask_state(uint8_t *bitmask, uint8_t channel, bool state);
static uint8_t extract_enabled_state(uint8_t *rx_buf, uint8_t channel);

struct sockaddr_un usbd_sock_addr;

struct tessel_device {
    struct ring_device *rings[N_CHANNEL];

    uint8_t channels_receivable_bitmask;
    uint8_t channels_writable_bitmask;
    uint8_t channels_opened_bitmask;
    uint8_t channels_enabled_bitmask;
    int retries;

    struct socket *servers[N_CHANNEL];

    int pin_irq_id;

    struct spi_device *dev;

    struct spi_message header_message;
    struct spi_transfer header_transfers[2];

    struct spi_message body_message;
    struct spi_transfer body_transfers[N_CHANNEL * 2];

    struct {
        int in_length;
        char in_buf[BUFSIZE];
        int out_length;
        char out_buf[BUFSIZE];
    } buffers[N_CHANNEL];
};

static struct tessel_device *tessel_dev;

static struct workqueue_struct *spi_workqueue;

static void tesseldev_queue_work(void);
static void tesseldev_prework(struct work_struct *);
static void tesseldev_work(struct work_struct *);

static void tesseldev_server_ready(struct sock *sk);

static DEFINE_SPINLOCK(spi_work_lock);
static bool spi_working = false;
static DECLARE_WORK(spi_prework, tesseldev_prework);
static DECLARE_WORK(spi_work, tesseldev_work);

static DEFINE_SPINLOCK(open_lock);

static int tesseldev_probe(struct spi_device *);
static int tesseldev_remove(struct spi_device *);

static int majorNumber;
static struct class* tesseldev_class = NULL;
static struct device* tesseldev_devices[N_CHANNEL];

static struct ring_file * __ring_init(struct ring_file *ring) {
    spin_lock_init(&ring->lock);

    if (kfifo_alloc(&ring->fifo, RING_FIFO_SIZE, GFP_KERNEL) != 0) {
        return NULL;
    }

    init_completion(&ring->wait_data);

    return ring;
}

static void __ring_release(struct ring_file *ring) {
    kfifo_free(&ring->fifo);
}

static struct ring_device * __ring_device_alloc(unsigned int minor) {
    unsigned long flags;
    bool unopened;
    struct ring_device *device = kzalloc(sizeof(struct ring_device), GFP_KERNEL);
    if (device == NULL) {
        goto err_alloc;
    }

    spin_lock_irqsave(&open_lock, flags);
    unopened = tessel_dev->rings[minor] == NULL;
    if (unopened) {
        tessel_dev->rings[minor] = device;
    }
    spin_unlock_irqrestore(&open_lock, flags);

    if (!unopened) {
        goto err_opened;
    }

    if (!__ring_init(&device->input)) {
        goto err_input;
    }
    if (!__ring_init(&device->output)) {
        goto err_output;
    }

    device->minor = minor;

    return device;

err_output:
    __ring_release(&device->input);
err_input:
err_opened:
    kfree(device);
err_alloc:
    return NULL;
}

static void __ring_device_free(struct ring_device *device) {
    unsigned long flags;
    unsigned int minor;

    minor = device->minor;

    spin_lock_irqsave(&open_lock, flags);
    tessel_dev->rings[minor] = NULL;
    spin_unlock_irqrestore(&open_lock, flags);

    __ring_release(&device->input);
    __ring_release(&device->output);
    if (device->socket) {
        sock_release(device->socket);
    }
    kfree(device);
}

static int ring_open(struct inode *inode, struct file *file) {
    struct ring_device *device = __ring_device_alloc(MINOR(inode->i_rdev));
    if (device == NULL) {
        return -1;
    }

    file->private_data = device;

    return 0;
}

static int ring_release(struct inode *inode, struct file *file) {
    struct ring_device *device = file->private_data;

    __ring_device_free(device);

    file->private_data = NULL;

    return 0;
}

static void __ring_socket_init(struct ring_device *device, struct socket *socket) {
    device->socket = socket;
    INIT_WORK(&device->socket_state_prework, ring_socket_prestate);
    INIT_WORK(&device->socket_state_work, ring_socket_state);
    INIT_WORK(&device->socket_read_prework, ring_socket_preread);
    INIT_WORK(&device->socket_read_work, ring_socket_read);
    INIT_WORK(&device->socket_write_prework, ring_socket_prewrite);
    INIT_WORK(&device->socket_write_work, ring_socket_write);
    INIT_WORK(&device->socket_cleanup_work, ring_socket_cleanup);

    memset(&device->socket_read, 0, sizeof(device->socket_read));
    device->socket_read.vec = (struct kvec) { &device->socket_read.body, BUFSIZE };

    socket->sk->sk_user_data = device;
    socket->sk->sk_state_change = ring_state_change;
    socket->sk->sk_data_ready = ring_data_ready;
    socket->sk->sk_write_space = ring_write_space;
}

static void __ring_socket_cleanup(struct ring_device *device) {
    device->socket->sk->sk_user_data = NULL;
    device->socket->sk->sk_state_change = ring_socket_noop;
    device->socket->sk->sk_data_ready = ring_socket_noop;
    device->socket->sk->sk_write_space = ring_socket_noop;

    cancel_work_sync(&device->socket_state_prework);
    cancel_work_sync(&device->socket_state_work);
    cancel_work_sync(&device->socket_read_prework);
    cancel_work_sync(&device->socket_read_work);
    cancel_work_sync(&device->socket_write_prework);
    cancel_work_sync(&device->socket_write_work);
}

static struct socket * ring_connect(unsigned int minor, struct sockaddr_un *addr, size_t addr_len, int flags) {
    int status;
    struct socket *socket = NULL;
    struct ring_device *device;

    status = sock_create_kern(PF_UNIX, SOCK_STREAM, 0, &socket);
    // Check for errors
    if (status < 0) {
        fatal("Error creating socket %s: %d\n", addr->sun_path, status);
        goto err_create;
    }

    // CONN_POLL(USBD_CHANNEL).fd = fd;

    // Connect to the USB Daemon
    status = kernel_connect(socket, (struct sockaddr *) addr, addr_len, flags);
    if (status < 0) {
        fatal("Error connecting to USB Daemon socket %s: %d\n", addr->sun_path, status);
        goto err_connect;
    }
    info("Connected socket on channel %d\n", minor);

    device = __ring_device_alloc(minor);
    if (device == NULL) {
        goto err_alloc;
    }

    __ring_socket_init(device, socket);

    return socket;

err_alloc:
err_connect:
    sock_release(socket);
err_create:
    return NULL;
}

static struct socket * ring_accept(unsigned int minor, struct socket *sock) {
    int status = 0;
    struct socket *socket = NULL;
    struct ring_device *device;

    status = kernel_accept(sock, &socket, O_NONBLOCK);
    // Nonblocking failure returns EAGAIN error. We can safely ignore this error
    // and try to accept again in the future as soon as we want like on some
    // event.
    if (status == -EAGAIN) {
        goto err;
    }
    if (status < 0) {
        fatal("Error accepting socket on channel %d: %d\n", minor, status);
        goto err;
    }
    info("Accepted socket on channel %d\n", minor);

    device = __ring_device_alloc(minor);
    if (device == NULL) {
        goto err_alloc;
    }

    __ring_socket_init(device, socket);

    return socket;

err_alloc:
    sock_release(socket);
err:
    return NULL;
}

static ssize_t ring_read(struct file *file, char __user *buf, size_t len, loff_t *pos) {
   if (access_ok(VERIFY_WRITE, buf, len)) {
       struct ring_device *device = file->private_data;
       ssize_t available = kfifo_len(&device->input.fifo);
       if (available == 0) {
           wait_for_completion(&device->input.wait_data);
       }
       else if (available >= RING_FIFO_SIZE - BUFSIZE) {
           tesseldev_queue_work();
       }

       debug("reading %d bytes from file on channel %d.\n", len, device->minor);
       return kfifo_out_locked(&device->input.fifo, buf, len, &device->input.lock);
   }
   else {
       return len;
   }
}

static ssize_t ring_write(struct file *file, const char __user *buf, size_t len, loff_t *pos) {
   if (access_ok(VERIFY_READ, buf, len)) {
       struct ring_device *device = file->private_data;
       ssize_t available = kfifo_len(&device->output.fifo);
       if (available == RING_FIFO_SIZE) {
           wait_for_completion(&device->output.wait_data);
       }
       tesseldev_queue_work();

       debug("writing %d bytes to file on channel %d.\n", len, device->minor);
       return kfifo_in_locked(&device->output.fifo, buf, len, &device->output.lock);
   }
   else {
       return len;
   }
}

static void ring_socket_noop(struct sock *sk) {}

static void ring_state_change(struct sock *sk) {
    struct ring_device *device = (struct ring_device *) sk->sk_user_data;
    if (!device) {
        return;
    }
    if (queue_work(spi_workqueue, &device->socket_state_work)) {
        queue_work(spi_workqueue, &device->socket_state_prework);
    }
}

static void ring_data_ready(struct sock *sk) {
    struct ring_device *device = (struct ring_device *) sk->sk_user_data;
    if (!device) {
        return;
    }
    if (queue_work(spi_workqueue, &device->socket_write_work)) {
        queue_work(spi_workqueue, &device->socket_write_prework);
    }
}

static void ring_write_space(struct sock *sk) {
    struct ring_device *device = (struct ring_device *) sk->sk_user_data;
    if (!device) {
        return;
    }
    if (queue_work(spi_workqueue, &device->socket_read_work)) {
        queue_work(spi_workqueue, &device->socket_read_prework);
    }
}

static void ring_socket_prestate(struct work_struct *work) {
    struct ring_device *device = container_of(work, struct ring_device, socket_state_prework);
    queue_work(spi_workqueue, &device->socket_state_work);
}

static void ring_socket_preread(struct work_struct *work) {
    struct ring_device *device = container_of(work, struct ring_device, socket_read_prework);
    queue_work(spi_workqueue, &device->socket_read_work);
}

static void ring_socket_prewrite(struct work_struct *work) {
    struct ring_device *device = container_of(work, struct ring_device, socket_write_prework);
    queue_work(spi_workqueue, &device->socket_write_work);
}

static void ring_socket_state(struct work_struct *work) {
    struct ring_device *device = container_of(work, struct ring_device, socket_state_work);

    info("channel %d socket state %d\n", device->minor, (int) device->socket->sk->sk_state);

    queue_work(spi_workqueue, &device->socket_write_work);
}

static void ring_socket_read(struct work_struct *work) {
    int status = 1;
    int len;
    struct msghdr msg = { .msg_flags = MSG_DONTWAIT | MSG_NOSIGNAL };
    struct ring_device *device = container_of(work, struct ring_device, socket_read_work);

    while (status > 0) {
        len = kfifo_len(&device->input.fifo);
        if (len > 0 && device->socket_read.len == 0) {
            device->socket_read.vec.iov_base = device->socket_read.body;
            device->socket_read.vec.iov_len = BUFSIZE;
            device->socket_read.len = kfifo_out(&device->input.fifo, device->socket_read.body, BUFSIZE);
            // if (!get_channel_bitmask_state(&tessel_dev->channels_writable_bitmask, chan)) {
            if (len > 0 && len <= BUFSIZE) {
                // set_channel_bitmask_state(&tessel_dev->channels_writable_bitmask, chan, true);
                tesseldev_queue_work();
            }
        }
        if (device->socket_read.len == 0) {
            break;
        }
        // else if (len > RING_FIFO_SIZE - BUFSIZE) {
        // }
        status = kernel_sendmsg(device->socket, &msg, &device->socket_read.vec, 1, device->socket_read.len);
        debug("sent %d bytes from socket on channel %d.\n", status, device->minor);
        if (status > 0) {
            device->socket_read.vec.iov_base += status;
            device->socket_read.vec.iov_len -= status;
            device->socket_read.len -= status;
        }
    }
}

static void ring_socket_write(struct work_struct *work) {
    int status = 1;
    int len;
    struct msghdr msg = { .msg_flags = MSG_DONTWAIT | MSG_NOSIGNAL };
    char body[BUFSIZE];
    struct kvec vec = { body, BUFSIZE };
    struct ring_device *device = container_of(work, struct ring_device, socket_write_work);

    while (status > 0 && kfifo_len(&device->output.fifo) == 0) {
        status = kernel_recvmsg(device->socket, &msg, &vec, 1, BUFSIZE, msg.msg_flags);
        debug("received %d bytes from socket on channel %d.\n", status, device->minor);
        if (status > 0) {
            len = kfifo_len(&device->output.fifo);
            kfifo_in(&device->output.fifo, body, status);
            if (get_channel_bitmask_state(&tessel_dev->channels_receivable_bitmask, device->minor)) {
                tesseldev_queue_work();
            }
            // if (len == 0) {
            // }
        }
        if (status == 0) {
            info("channel %d closed remotely\n", device->minor);
            queue_work(spi_workqueue, &device->socket_cleanup_work);
        }
        if (status == -ECONNRESET) {
            error("channel %d forcefully closed remotely\n", device->minor);
            queue_work(spi_workqueue, &device->socket_cleanup_work);
        }
    }
}

static void ring_socket_cleanup(struct work_struct *work) {
    uint8_t channel;
    struct ring_device *device = container_of(work, struct ring_device, socket_cleanup_work);
    channel = device->minor;
    __ring_socket_cleanup(device);
    __ring_device_free(device);

    // Set the channel open status to false
    set_channel_bitmask_state(&tessel_dev->channels_opened_bitmask, channel, false);

    // Notify the MCU that this channel has closed.
    tesseldev_queue_work();
}

static irqreturn_t tesseldev_irq(int cpl, void *dev_id) {
    debug("irq pin has risen.\n");
    tesseldev_queue_work();
    return IRQ_HANDLED;
}

static bool ring_putable(struct ring_device *device) {
    if (device == NULL) {
        return false;
    }
    return kfifo_len(&device->input.fifo) == 0;
    // return kfifo_avail(&device->input.fifo) > BUFSIZE;
}

static ssize_t ring_len(struct ring_device *device) {
    if (device == NULL) {
        return 0;
    }
    return min(kfifo_len(&device->output.fifo), (size_t) BUFSIZE);
}

static ssize_t ring_put(struct ring_device *device, unsigned char *buf, size_t len) {
    size_t old_len = kfifo_len(&device->input.fifo);
    ssize_t result = kfifo_in(&device->input.fifo, buf, len);
    if (old_len == 0) {
        complete_all(&device->input.wait_data);
    }
    if (device->socket) {
        queue_work(spi_workqueue, &device->socket_read_work);
    }
    return result;
}

// static void __ring_spi_add(struct ring_device *device, struct spi_message *message, struct spi_transfer **transfers, size_t len, void *tx_buf, void *rx_buf) {
//     struct spi_transfer *transfer = *transfers;
//     transfer->len = len;
//     transfer->tx_buf = tx_buf;
//     transfer->rx_buf = rx_buf;
//     transfer->speed_hz = device->minor == USBD_CHANNEL ? 10000000 : 0;
//     spi_message_add_tail(transfer, message);
//     (*transfers)++;
// }

// static size_t tesselring_spi_put_prepare(struct ring_device *device, struct spi_message *message, struct spi_transfer *transfers, size_t len) {
//     size_t desc = 0;
//
//     // struct __kfifo *fifo = &device->input.fifo.kfifo;
//     // size_t size = fifo->mask + 1;
//     // size_t off = fifo->in & fifo->mask;
//     // size_t l = min(len, size - off);
//     // device->spi_put_len = len;
//     //
//     // // __ring_spi_add(device, message, &transfers, l, NULL, fifo->data + off);
//     // struct spi_transfer *transfer = transfers;
//     // transfer->len = l;
//     // transfer->rx_buf = fifo->data + off;
//     // transfer->speed_hz = device->minor == USBD_CHANNEL ? 10000000 : 0;
//     // spi_message_add_tail(transfer, message);
//     // transfers++;
//     // desc++;
//     //
//     // if (l != len) {
//     //     // __ring_spi_add(device, message, &transfers, len - l, NULL, fifo->data);
//     //     transfer = transfers;
//     //     transfer->len = len - l;
//     //     transfer->rx_buf = fifo->data;
//     //     transfer->speed_hz = device->minor == USBD_CHANNEL ? 10000000 : 0;
//     //     spi_message_add_tail(transfer, message);
//     //     transfers++;
//     //     desc++;
//     // }
//
//     return desc;
// }

// static void ring_spi_put_finish(struct ring_device *device) {
//     struct __kfifo *fifo = &device->input.fifo.kfifo;
//     size_t old_len = fifo->in - fifo->out;
//     fifo->in += device->spi_put_len;
//     if (old_len == 0) {
//         complete_all(&device->input.wait_data);
//     }
//     if (device->socket) {
//         queue_work(spi_workqueue, &device->socket_read_work);
//     }
// }

static ssize_t ring_get(struct ring_device *device, unsigned char *buf, size_t len) {
    size_t old_len = kfifo_len(&device->output.fifo);
    ssize_t result = kfifo_out(&device->output.fifo, buf, len);
    if (RING_FIFO_SIZE - old_len < BUFSIZE) {
        complete_all(&device->output.wait_data);
    }
    if (device->socket) {
        queue_work(spi_workqueue, &device->socket_write_work);
    }
    return result;
}

// static size_t ring_spi_get_prepare(struct ring_device *device, struct spi_message *message, struct spi_transfer *transfers, size_t len) {
//     size_t desc = 0;
//
//     struct __kfifo *fifo = &device->output.fifo.kfifo;
//     size_t size = fifo->mask + 1;
//     size_t off = fifo->out & fifo->mask;
//     size_t l = min(len, size - off);
//     device->spi_get_len = len;
//
//     __ring_spi_add(device, message, &transfers, l, fifo->data + off, NULL);
//     desc++;
//
//     if (l != len) {
//         __ring_spi_add(device, message, &transfers, len - l, fifo->data, NULL);
//         desc++;
//     }
//
//     return desc;
// }
//
// static void ring_spi_get_finish(struct ring_device *device) {
//     struct __kfifo *fifo = &device->output.fifo.kfifo;
//     size_t old_len = fifo->in - fifo->out;
//     fifo->out += device->spi_get_len;
//     if (old_len > RING_FIFO_SIZE - BUFSIZE) {
//         complete_all(&device->output.wait_data);
//     }
//     if (device->socket) {
//         queue_work(spi_workqueue, &device->socket_write_work);
//     }
// }

/*
Fetches the stored open/closed state of a given channel

Args:
    - bitmask: the bitmask to fetch a value from
    - channel: the index of the channel to check the status of

Returns:
    STATUS_TRUE if state is currently active
    STATUS_FALSE if state is currently inactive

*/
static uint8_t get_channel_bitmask_state(uint8_t *bitmask, uint8_t channel) {
    return ((*bitmask) & (1 << channel)) ? STATUS_TRUE : STATUS_FALSE;
}

/*
Sets a channel bitmap state

Args:
    - bitmask: the bitmask to modify
    - channel: the index of the channel to set the state of
    - state: a bool determining whether that state is active or not

*/
static void set_channel_bitmask_state(uint8_t *bitmask, uint8_t channel, bool state) {
    if (state == true) {
        *(bitmask) |= (1 << channel);
    }
    else {
        *(bitmask) &= ~(1 << channel);
    }
}

/*
Helper function to pull out the correct bitmask from a buffer header sent by the coprocessor

Args:
    rx_buf: The buffer sent from the coprocessor
    channel: The connection channel to get the enabled status of
*/
static uint8_t extract_enabled_state(uint8_t *rx_buf, uint8_t channel) {
    return ((rx_buf[STATUS_BYTE] & (STATUS_BIT << channel)) ? STATUS_TRUE : STATUS_FALSE);
}
/*
Closes a provided channel's connection

Args:
    - channel: the index of the channel
        0: USB
        1: MODULE PORT A
        2: MODULE PORT B

*/
static void close_channel_connection(uint8_t channel) {
    info("Closing connection %d\n", channel);
    // We can only release socket rings.
    if (tessel_dev->rings[channel] && tessel_dev->rings[channel]->socket) {
        __ring_socket_cleanup(tessel_dev->rings[channel]);
        __ring_device_free(tessel_dev->rings[channel]);
    }

    // Set the channel open status to false
    set_channel_bitmask_state(&tessel_dev->channels_opened_bitmask, channel, false);
    // // Set the writability to false
    // set_channel_bitmask_state(&tessel_dev->channels_writable_bitmask, channel, false);
}

static bool enable_usb_daemon_socket(void) {
    // bool opened = false;
    // Create a new socket
    bool opened = ring_connect(0, &usbd_sock_addr, sizeof(usbd_sock_addr), 0) != NULL;

    // // Set the bits of the events we want to listen to
    // CONN_POLL(USBD_CHANNEL).events = POLLIN | POLLOUT | POLLERR;

    // Mark the channel as opened
    set_channel_bitmask_state(&tessel_dev->channels_opened_bitmask, USBD_CHANNEL, opened);

    return opened;
}

static void tesseldev_queue_work() {
    if (!spi_working) {
        return;
    }

    if (queue_work(spi_workqueue, &spi_work)) {
        queue_work(spi_workqueue, &spi_prework);
    }
}

static void tesseldev_prework(struct work_struct *work) {
    // if (gpio_get_value(pin_irq) == 0 &&
    //     ring_len(tessel_dev->rings[0]) == 0 &&
    //     ring_len(tessel_dev->rings[1]) == 0 &&
    //     ring_len(tessel_dev->rings[2]) == 0
    // ) {
    //     return;
    // }

    queue_work(spi_workqueue, &spi_work);
}

static void tesseldev_work(struct work_struct *work) {
    int i;
    unsigned long start;

    if (!spi_working) {
        return;
    }

    // do the spid sync work
    for (;;) {
        uint8_t tx_buf[2 + N_CHANNEL];
        uint8_t rx_buf[2 + N_CHANNEL];
        int status;
        int desc;
        int chan;

        debug("pin_irq: %d\n", gpio_get_value(pin_irq));
        gpio_get_value(pin_irq);
        // if (gpio_get_value(pin_irq) < 0) {
        //     break;
        // }

        // if (gpio_get_value(pin_irq) == 0 &&
        //     ring_len(tessel_dev->rings[0]) == 0 &&
        //     ring_len(tessel_dev->rings[1]) == 0 &&
        //     ring_len(tessel_dev->rings[2]) == 0
        // ) {
        //     break;
        // }

        gpio_set_value(pin_sync, 0);

        // Wait for MCU to set up its SPI hardware
        udelay(10);

        spi_message_init(&tessel_dev->header_message);
        memset(tessel_dev->header_transfers, 0, sizeof(tessel_dev->header_transfers));
        memset(rx_buf, 0, sizeof(rx_buf));

        for (i = 0; i < N_CHANNEL; i++) {
            // set_channel_bitmask_state(&tessel_dev->channels_opened_bitmask, i, tessel_dev->rings[i] != NULL);
            set_channel_bitmask_state(
                &tessel_dev->channels_writable_bitmask,
                i,
                ring_putable(tessel_dev->rings[i]) &&
                    get_channel_bitmask_state(&tessel_dev->channels_opened_bitmask, i)
                );
            tessel_dev->buffers[i].out_length = ring_len(tessel_dev->rings[i]);
            tx_buf[2 + i] = tessel_dev->buffers[i].out_length;
            // if (i > 0) {
            //     tx_buf[2 + i] = 0;
            // }
            // tx_buf[2 + i] = 0;
        }

        tx_buf[0] = 0x53;
        tx_buf[1] = tessel_dev->channels_writable_bitmask | (tessel_dev->channels_opened_bitmask << 4);

        debug("tx: %2x %2x %2x %2x %2x\n", tx_buf[0], tx_buf[1], tx_buf[2], tx_buf[3], tx_buf[4]);

        tessel_dev->header_transfers[0].len = sizeof(tx_buf);
        tessel_dev->header_transfers[0].tx_buf = tx_buf;
        // tessel_dev->header_transfers[0].delay_usecs = 2;
        spi_message_add_tail(&tessel_dev->header_transfers[0], &tessel_dev->header_message);
        tessel_dev->header_transfers[1].len = sizeof(rx_buf);
        tessel_dev->header_transfers[1].rx_buf = rx_buf;
        // tessel_dev->header_transfers[1].delay_usecs = 2;
        spi_message_add_tail(&tessel_dev->header_transfers[1], &tessel_dev->header_message);
        // start = jiffies;
        debug("rx: %2x %2x %2x %2x %2x\n", rx_buf[0], rx_buf[1], rx_buf[2], rx_buf[3], rx_buf[4]);
        status = spi_sync(tessel_dev->dev, &tessel_dev->header_message);
        // debug("Header message time %d %d\n", (jiffies - start), HZ);

        if (status < 0) {
            fatal("Failed to sync spi header message to MCU");
            break;
        }

        debug("rx: %2x %2x %2x %2x %2x\n", rx_buf[0], rx_buf[1], rx_buf[2], rx_buf[3], rx_buf[4]);
        if (rx_buf[0] != 0xCA) {
            error("Invalid command reply: %2x %2x %2x %2x %2x\n", rx_buf[0], rx_buf[1], rx_buf[2], rx_buf[3], rx_buf[4]);
            tessel_dev->retries++;

            // Set high so next setting 0 triggers the MCU to load headers.
            gpio_set_value(pin_sync, 1);
            udelay(10);

            if (tessel_dev->retries > 15) {
                fatal("Too many retries, exiting");
                tessel_dev->retries = 0;
                break;
            } else {
                continue;
            }
        }

        // For each possible channel
        for (i = 0; i < N_CHANNEL; i++) {
            // Extract the new channel enabled status from the packet header
            uint8_t new_status = extract_enabled_state(rx_buf, i);
            // Fetch the old enabled status
            uint8_t old_status = get_channel_bitmask_state(&tessel_dev->channels_enabled_bitmask, i);
            debug("Channel %d, old enabled status: %d, new enabled status: %d", i, old_status, new_status);
            // If the status hasn't changed
            if (new_status == old_status) {
                debug("Status has not changed.\n");
                if (old_status && !get_channel_bitmask_state(&tessel_dev->channels_opened_bitmask, i)) {
                    if (i == USBD_CHANNEL) {
                        enable_usb_daemon_socket();
                    }
                    else if (tessel_dev->servers[i]) {
                        bool opened = ring_accept(i, tessel_dev->servers[i]) != NULL;
                        // Mark the channel as opened
                        set_channel_bitmask_state(&tessel_dev->channels_opened_bitmask, i, opened);
                    }
                }
                // Make no changes to the polling
                continue;
            }
            // If the new status has the channel enabled
            else if (new_status == STATUS_TRUE) {
                debug("Channel has been enabled!\n");
                if (i == USBD_CHANNEL) {
                    enable_usb_daemon_socket();
                }
                else if (tessel_dev->servers[i]) {
                    bool opened = ring_accept(i, tessel_dev->servers[i]) != NULL;
                    // Mark the channel as opened
                    set_channel_bitmask_state(&tessel_dev->channels_opened_bitmask, i, opened);
                }
                // Set the status as enabled
                set_channel_bitmask_state(&tessel_dev->channels_enabled_bitmask, i, true);
            }
            // If the new status disables the channel
            else {
                debug("Channel has been disabled!\n");
                // Close the socket and mark the channel closed
                close_channel_connection(i);
                // Mark the channel as disabled
                set_channel_bitmask_state(&tessel_dev->channels_enabled_bitmask, i, false);
            }
        }

        gpio_set_value(pin_sync, 1);
        udelay(10);

        spi_message_init(&tessel_dev->body_message);
        memset(tessel_dev->body_transfers, 0, sizeof(tessel_dev->body_transfers));
        debug("size of body_transfers %d %d\n", sizeof(struct spi_transfer), sizeof(tessel_dev->body_transfers));

        desc = 0;

        for (chan = 0; chan < N_CHANNEL; chan++) {
            int size = tessel_dev->buffers[chan].out_length;
            // If the coprocessor is ready to receive, and we have data to send
            if (rx_buf[1] & (1 << chan) && size > 0) {
                debug("coprocessor is ready to receive and we have %d bytes from channel %d", size, chan);
                // // Make this channel readable by others
                // CONN_POLL(chan).events |= POLLIN;
                // Set the length to the size we need to send
                tessel_dev->body_transfers[desc].len = size;
                // Fill the out buffer from the character device.
                ring_get(tessel_dev->rings[chan], tessel_dev->buffers[chan].out_buf, size);
                // Point the output buffer to the correct place
                tessel_dev->body_transfers[desc].tx_buf = &tessel_dev->buffers[chan].out_buf[0];
                // Note that we will have no more data to send (once this is sent)
                // channels[chan].out_length = 0;

                tessel_dev->body_transfers[desc].speed_hz = chan == USBD_CHANNEL ? 10000000 : 0;

                // tessel_dev->body_transfers[desc].speed_hz = 10000000;
                // Mark that we need to make a SPI transaction
                spi_message_add_tail(&tessel_dev->body_transfers[desc], &tessel_dev->body_message);
                desc++;
                set_channel_bitmask_state(&tessel_dev->channels_receivable_bitmask, chan, false);
            }
            else if (rx_buf[1] & (1 << chan)) {
                set_channel_bitmask_state(&tessel_dev->channels_receivable_bitmask, chan, true);
            }

            // The number of bytes the coprocessor wants to send to a channel
            size = rx_buf[2 + chan];
            // Check that the channel is writable and there is data that needs to be received
            if (get_channel_bitmask_state(&tessel_dev->channels_writable_bitmask, chan) && size > 0) {
                debug("Channel %d is ready to have %d bytes written to it from bridge", chan, size);
                // Set the appropriate size
                tessel_dev->body_transfers[desc].len = size;
                // Point our receive buffer to the in buf of the appropriate channel
                tessel_dev->body_transfers[desc].rx_buf = &tessel_dev->buffers[chan].in_buf[0];
                tessel_dev->body_transfers[desc].speed_hz = chan == USBD_CHANNEL ? 10000000 : 0;
                // tessel_dev->body_transfers[desc].speed_hz = 10000000;
                // Mark that we need a SPI transaction to take place
                spi_message_add_tail(&tessel_dev->body_transfers[desc], &tessel_dev->body_message);
                desc++;
            }
        }

        // If the previous logic designated the need for a SPI transaction
        if (desc != 0) {

            // Wait for MCU to set up its SPI hardware
            udelay(10 * desc);
            // udelay(desc >= 1 ? 5 * (desc - 1) : 0);

            debug("Performing transfer on %i channels\n", desc);

            // Make the SPI transaction
            // start = jiffies;
            status = spi_sync(tessel_dev->dev, &tessel_dev->body_message);
            // debug("Body message %d time %d %d\n", desc, (jiffies - start), HZ);

            // Ensure there were no errors
            if (status < 0) {
                fatal("SPI_IOC_MESSAGE: data: %d", status);
                break;
            }

            // Write received data to the appropriate socket
            for (chan = 0; chan < N_CHANNEL; chan++) {
                // Get the length of the received data for this channel
                int size = rx_buf[2 + chan];
                // Make sure that channel is writable and we have data to send to it
                if (get_channel_bitmask_state(&tessel_dev->channels_writable_bitmask, chan) && size > 0) {
                    // Write this data to the pipe
                    int r = ring_put(tessel_dev->rings[chan], &tessel_dev->buffers[chan].in_buf[0], size);
                    // int r = 0;
                    debug("%i: Write %u %i\n", chan, size, r);
                    // Ensure there were no errors
                    if (r < 0) {
                        error("Error in write %i: %d\n", chan, r);
                        break;
                    }

                    // // Mark we want to know when this pipe is writable again
                    // CONN_POLL(chan).events |= POLLOUT;
                    // // Set the state to not writable
                    // set_channel_bitmask_state(&tessel_dev->channels_writable_bitmask, chan, false);
                }
            }
        }
        else {
            break;
        }

        // Wait for MCU flag if it still wants to communicate
        udelay(10);

        break;
    }
}

static void tesseldev_server_ready(struct sock *sk) {
    tesseldev_queue_work();
}

static int tesseldev_probe(struct spi_device *spi) {
    int status = 0;
    int retval;
    int save;
    int i;

    save = spi->max_speed_hz;
    spi->max_speed_hz = 20000000;
    retval = spi_setup(spi);
    if (retval < 0)
        spi->max_speed_hz = save;

    info("%d Hz (max)\n", spi->max_speed_hz);

    // Register the device drivers (usb, port_a, port_b)
    for (i = 0; i < N_CHANNEL; i++) {
        tesseldev_devices[i] = device_create(tesseldev_class, NULL, MKDEV(majorNumber, i), NULL, DEVICE_NAME "%d", i);
        if (IS_ERR(tesseldev_devices[i])) {
            printk(KERN_ALERT "Failed to create the device\n");
            status = PTR_ERR(tesseldev_devices[i]);
            goto err_device;
        }
        printk(KERN_INFO "TesselDev: device class created correctly\n");
    }

    tessel_dev->dev = spi;

    spi_workqueue = alloc_workqueue("%s", WQ_UNBOUND | WQ_MEM_RECLAIM | WQ_HIGHPRI, 1, "tesseldev_spi");
    if (IS_ERR(spi_workqueue)) {
        status = PTR_ERR(spi_workqueue);
        goto err_thread;
    }

    spi_working = true;
    tesseldev_queue_work();

    return status;

err_thread:
    tessel_dev->dev = NULL;
err_device:
    for (i--; i >= 0; i--) {
        device_destroy(tesseldev_class, MKDEV(majorNumber, i));
    }
    return status;
}

static int tesseldev_remove(struct spi_device *spi) {
    int i;

    spi_working = false;
    destroy_workqueue(spi_workqueue);

    tessel_dev->dev = NULL;

    for (i = 0; i < N_CHANNEL; i++) {
        device_destroy(tesseldev_class, MKDEV(majorNumber, i));
    }

    return 0;
}

struct file_operations ring_ops = {
    .read = ring_read,
    .write = ring_write,
    // .poll = ring_poll,
    .open = ring_open,
    .release = ring_release,
    .llseek = no_llseek,
};

static struct spi_driver tesseldev_spi_driver = {
    .driver = {
        .name = DEVICE_NAME,
    },
    .probe = tesseldev_probe,
    .remove = tesseldev_remove,
};

static int __init tesseldev_init(void) {
    int status;
    int i;

    spi_working = false;

    // create character class
    info("Initializing the TesselDev LKM\n");

    tessel_dev = kzalloc(sizeof(struct tessel_device), GFP_KERNEL);
    if (tessel_dev == NULL) {
        status = -ENOMEM;
        goto err_alloc;
    }

    // Try to dynamically allocate a major number for the device -- more
    // difficult but worth it
    majorNumber = register_chrdev(0, DEVICE_NAME, &ring_ops);
    if (majorNumber<0){
        printk(KERN_ALERT "TesselDev failed to register a major number\n");
        status = -1;
        goto err_chrdev;
    }
    printk(KERN_INFO "TesselDev: registered correctly with major number %d\n", majorNumber);

    // Register the device class
    tesseldev_class = class_create(THIS_MODULE, CLASS_NAME);
    if (IS_ERR(tesseldev_class)) {
        printk(KERN_ALERT "Failed to register device class\n");
        status = PTR_ERR(tesseldev_class);
        goto err_class;
    }
    printk(KERN_INFO "TesselDev: device class registered correctly\n");

    // mm_segment_t old_fs;
    // old_fs = get_fs();
    // set_fs(get_ds());
    // char * socket_dir_dup = kmalloc(sizeof(SOCKET_DIR) + 1, GFP_KERNEL);
    // snprintf(socket_dir_dup, sizeof(SOCKET_DIR) + 1, "%s/", SOCKET_DIR);
    // info("creating %s directory\n", socket_dir_dup);
    // int retval = sys_mkdir(socket_dir_dup, 0755);
    // if (retval < 0) {
    //     error("couldn't create %s directory: code %d\n", socket_dir_dup, retval);
    // }
    // struct file *_socket_dir = NULL;
    // _socket_dir = filp_open(socket_dir_dup, O_RDONLY | O_CREAT | O_DIRECTORY, 0700);
    // if (IS_ERR(_socket_dir)) {
    //     error("couldn't open %s directory %d\n", socket_dir_dup, PTR_ERR(_socket_dir));
    //     goto err_open;
    // }
    // filp_close(_socket_dir, NULL);
// err_open:
//     kfree(socket_dir_dup);
//     set_fs(old_fs);

    // Create the listening unix domain sockets
    for (i = 0; i < N_CHANNEL; i++) {

        // If this is not the USB Daemon channel
        if (i != USBD_CHANNEL) {
            struct socket *sock;
            // Create a struct to store socket info
            struct sockaddr_un addr;
            // Use UNIX family sockets
            addr.sun_family = AF_UNIX;
            // Copy the path of the socket into the struct
            snprintf(addr.sun_path, sizeof(addr.sun_path), "%s/tessel%d", "/var/run", i);

        //     mm_segment_t old_fs;
        //     old_fs = get_fs();
        //     set_fs(get_ds());
        //
        //     struct file * _socket_file = filp_open(addr.sun_path, O_CREAT, 0644);
        //     if (IS_ERR(_socket_file)) {
        //         error("couldn't create %s socket file: code %d\n", addr.sun_path, PTR_ERR(_socket_file));
        //         goto err_open_file;
        //     }
        //     filp_close(_socket_file, NULL);
        //     info("created %s socket file\n", addr.sun_path);
        // err_open_file:
        //     set_fs(old_fs);

            // Create the socket
            int retval = sock_create_kern(PF_UNIX, SOCK_STREAM, 0, &sock);
            // int fd = socket(addr.sun_family, SOCK_STREAM, 0);
            // Check for errors
            if (retval < 0) {
                fatal("Error creating socket %s: %d\n", addr.sun_path, retval);
                goto err_sock_create;
            }

            tessel_dev->servers[i] = sock;

            // // Delete any previous paths because we'll create a new one
            // unlink(addr.sun_path);

            // Bind to that socket address
            retval = kernel_bind(sock, (struct sockaddr *) &addr, sizeof(addr));
            if (retval < 0) {
                fatal("Error binding socket %s: %d\n", addr.sun_path, retval);
                goto err_bind;
            }

            // Start listening for new connections
            retval = kernel_listen(sock, 1);
            if (retval < 0) {
                fatal("Error listening on socket %s: %d\n", addr.sun_path, retval);
                goto err_listen;
            }

            sock->sk->sk_state_change = tesseldev_server_ready;
            sock->sk->sk_data_ready = tesseldev_server_ready;

            continue;

        err_listen:
        err_bind:
            sock_release(sock);
            tessel_dev->servers[i] = NULL;
        err_sock_create:
            continue;
        }
        // If this is the USB Daemon channel
        else {
            // Set the family of our global addr struct
            usbd_sock_addr.sun_family = AF_UNIX;
            // Copy the addr info into a global
            snprintf(usbd_sock_addr.sun_path, sizeof(usbd_sock_addr.sun_path), "%s/%s", SOCKET_DIR, "usb");
        }
    }

    // set up IRQ pin
    status = gpio_request(pin_irq, "mcu_async");
    if (status < 0) {
        goto err_gpio_irq;
    }

    status = gpio_direction_input(pin_irq);
    if (status < 0) {
        goto err_gpio_irq_input;
    }

    // set up sync pin
    status = gpio_request(pin_sync, "mcu_sync");
    if (status < 0) {
        goto err_gpio_sync;
    }

    status = gpio_direction_output(pin_sync, GPIOF_INIT_HIGH);
    if (status < 0) {
        goto err_gpio_sync_output;
    }

    tessel_dev->pin_irq_id = gpio_to_irq(pin_irq);
    status = request_irq(tessel_dev->pin_irq_id,
        tesseldev_irq,
        IRQF_TRIGGER_RISING,
        "tesseldev_irq",
        NULL);
    if (status < 0) {
        goto err_gpio_irq_request;
    }

    status = spi_register_driver(&tesseldev_spi_driver);
    if (status < 0) {
        goto err_spi_driver;
    }

    return 0;

err_spi_driver:
    free_irq(tessel_dev->pin_irq_id, NULL);
err_gpio_irq_request:
    tessel_dev->pin_irq_id = 0;
err_gpio_sync_output:
    gpio_free(pin_sync);
err_gpio_sync:
err_gpio_irq_input:
    gpio_free(pin_irq);
err_gpio_irq:
    for (i = 0; i < N_CHANNEL; i++) {
        if (i != USBD_CHANNEL && tessel_dev->servers[i] != NULL) {
            sock_release(tessel_dev->servers[i]);
        }
    }
    class_destroy(tesseldev_class);
err_class:
    unregister_chrdev(majorNumber, DEVICE_NAME);
err_chrdev:
    kfree(tessel_dev);
err_alloc:
    return status;
}

static void __exit tesseldev_exit(void) {
    int i;

    spi_unregister_driver(&tesseldev_spi_driver);
    free_irq(tessel_dev->pin_irq_id, NULL);
    tessel_dev->pin_irq_id = 0;
    gpio_free(pin_sync);
    gpio_free(pin_irq);
    for (i = 0; i < N_CHANNEL; i++) {
        if (i != USBD_CHANNEL && tessel_dev->servers[i] != NULL) {
            sock_release(tessel_dev->servers[i]);
        }
    }
    class_destroy(tesseldev_class);
    unregister_chrdev(majorNumber, DEVICE_NAME);
    kfree(tessel_dev);

    spi_working = false;

    printk(KERN_INFO "TesselDev: exited\n");
}

module_init(tesseldev_init);
module_exit(tesseldev_exit);

// The license type -- Some kernel functions are only available to free
// software. The kernel strictly checks the MODULE_LICENSE for wahat kernel
// functions may be available to it.
MODULE_LICENSE("Dual MIT/GPL");
MODULE_AUTHOR("Michael \"Z\" Goddard");
MODULE_DESCRIPTION("A spidev-like device that communicates with the Tessel 2 MCU");
MODULE_VERSION("0.1");
