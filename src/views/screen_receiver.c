/* File > Show calculator's screen: shows a real calculator's screen in the
 * window, as Casio's Screen Receiver does. It takes either of two links:
 *
 * - The OS's "Screen Receiver" (or "Projector") mode: the fx-CG is a USB
 *   drive (07cf:6102, interface class 08) that also answers three vendor
 *   SCSI commands: C0 (how many bytes are waiting), C1 (read them) and C2
 *   (write). What it sends is Protocol 7.00 screenstreaming
 *   (https://cahute.org/topics/protocols/seven-ohp.html): check packets
 *   (16 "CAL00" "D0") until the receiver acknowledges one (06 "02001" "0D"),
 *   then a frame packet each time the screen changes.
 * - A gint add-in with its USB link open (usb_open with usb_ff_bulk, class
 *   ff): it sends fxlink messages on a bulk endpoint, and the "video" and
 *   "image" ones carry frames (usb_fxlink_videocapture, as a dupdate hook).
 *   Starting such an add-in on the calculator ends Screen Receiver mode; the
 *   calculator comes back as the fxlink device.
 *
 * Until it is stopped it waits for a calculator, and again whenever one goes
 * away, so add-ins can be started and left while it runs.
 *
 * Linux: the device is driven through usbfs (/dev/bus/usb/BBB/DDD, which
 * needs a udev rule giving the user access), taking it from the kernel's
 * usb-storage driver for the time and giving it back after. A thread does
 * the USB work; frames reach the window through receiver_poll(). Every step
 * is logged on the terminal ("screen receiver: ..."). */
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "r61524.h"
#include "view.h"

#ifdef __linux__
#include <dirent.h>
#include <fcntl.h>
#include <linux/usbdevice_fs.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif

#define CASIO_VENDOR  0x07cf
#define USB_FLASH_PID 0x6103 /* "USB Flash": a plain drive, no screen */
#define POLL_IDLE_MS  4      /* between C0 polls while nothing is waiting */
#define USB_TIMEOUT   2000   /* per bulk transfer, ms */
#define READ_TIMEOUT  250    /* per fxlink read: how soon a stop is noticed */
#define RETRY_MS      500    /* between looks for a calculator */
#define STREAM_CAP    (1 << 20)
#define FXLINK_CHUNK  16384  /* per usbfs bulk read */

enum { R_OFF, R_RUNNING, R_ENDED };

static SDL_Thread *thread;
static SDL_AtomicInt state;      /* R_* */
static SDL_AtomicInt stop;       /* the window asks the thread to finish */
static SDL_AtomicInt frame_new;  /* panel holds a frame not shown yet */
static SDL_AtomicInt note_new;   /* note holds a message not shown yet */
static SDL_Mutex *lock;          /* panel and note */
static uint16_t panel[PANEL_WIDTH * PANEL_HEIGHT];
static char note[300];           /* for the menu bar: waiting, connected... */
static char ended_why[600];      /* why the thread gave up, for the window */
static bool was_paused;
static Uint32 wake_event;        /* pushed with each frame, to wake the window */
static unsigned frames;          /* in this connection */
static char told[sizeof note];   /* the last note, not repeated */

static void LOG(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "screen receiver: ");
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

static void wake(void)
{
    SDL_Event e = {.type = wake_event};
    SDL_PushEvent(&e);
}

/* A message for the menu bar, unless it is the one shown last. */
static void tell(const char *msg)
{
    if (!strcmp(msg, told))
        return;
    snprintf(told, sizeof told, "%s", msg);
    LOG("%s", msg);
    SDL_LockMutex(lock);
    snprintf(note, sizeof note, "%s", msg);
    SDL_UnlockMutex(lock);
    SDL_SetAtomicInt(&note_new, 1);
    wake();
}

/*---------------------------------------------------------------------------
 * Frames
 *-------------------------------------------------------------------------*/

static uint16_t rgb(unsigned r, unsigned g, unsigned b) /* 8 bits each */
{
    return (uint16_t)((r >> 3) << 11 | (g >> 2) << 5 | b >> 3);
}

/* The size a picture of format ff should have, or -1 for an unknown one. */
static long picture_size(const char ff[4], long w, long h)
{
    if (!memcmp(ff, "1RC2", 4))
        return w * h * 2;
    if (!memcmp(ff, "1RC3", 4))
        return (w * h + 1) / 2;
    if (!memcmp(ff, "1RM2", 4))
        return 2 * h * ((w + 7) / 8);
    return -1;
}

/* A picture (formats as screenstreaming names them), centred on the panel:
 * the OS's 384x216 lands where its VRAM does, gint's 396x224 fills it; the
 * rest white. Hands it to the window. */
static void decode(const uint8_t *f, int w, int h, const char ff[4])
{
    static uint16_t out[PANEL_WIDTH * PANEL_HEIGHT];
    for (int i = 0; i < PANEL_WIDTH * PANEL_HEIGHT; i++)
        out[i] = 0xFFFF;
    int x0 = (PANEL_WIDTH - w) / 2, y0 = (PANEL_HEIGHT - h) / 2;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            long i = (long)y * w + x;
            uint16_t c;
            if (!memcmp(ff, "1RC2", 4)) {        /* R5G6B5, big-endian */
                c = (uint16_t)(f[2 * i] << 8 | f[2 * i + 1]);
            } else if (!memcmp(ff, "1RC3", 4)) { /* 4 bits: R G B, unused (high nibble first: a guess) */
                int v = i & 1 ? f[i / 2] & 15 : f[i / 2] >> 4;
                c = rgb(v & 8 ? 255 : 0, v & 4 ? 255 : 0, v & 2 ? 255 : 0);
            } else {                             /* 1RM2: two 1-bit planes, 4 greys */
                long stride = (w + 7) / 8, plane = stride * h;
                int bit = 7 - (x & 7);
                int a = f[y * stride + x / 8] >> bit & 1, b = f[plane + y * stride + x / 8] >> bit & 1;
                static const uint8_t GREY[4] = {0xFF, 0xAA, 0x77, 0x00}; /* index a*2+b */
                c = rgb(GREY[a * 2 + b], GREY[a * 2 + b], GREY[a * 2 + b]);
            }
            out[(y0 + y) * PANEL_WIDTH + x0 + x] = c;
        }
    SDL_LockMutex(lock);
    memcpy(panel, out, sizeof panel);
    SDL_UnlockMutex(lock);
    SDL_SetAtomicInt(&frame_new, 1);
    wake();
    if (frames++ == 0)
        tell("Showing the calculator's screen");
}

/*---------------------------------------------------------------------------
 * Screenstreaming packets (Screen Receiver, Projector)
 *-------------------------------------------------------------------------*/

/* n ASCII hex digits, or -1. */
static long hex(const uint8_t *p, int n)
{
    long v = 0;
    for (int i = 0; i < n; i++) {
        int c = p[i], d = c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10
                        : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
        if (d < 0)
            return -1;
        v = v * 16 + d;
    }
    return v;
}

/* The checksum of bytes 1..n-1 of a packet n bytes long (the two
 * checksum characters follow): the two's complement of their sum. */
static bool checksum_ok(const uint8_t *p, size_t n)
{
    unsigned sum = 0;
    for (size_t i = 1; i < n; i++)
        sum += p[i];
    return hex(p + n, 2) == (long)((~sum + 1) & 0xFF);
}

typedef enum { P_NEED_MORE, P_BAD, P_CHECK, P_FRAME, P_OTHER } Parsed;

/* The packet at the start of s (n bytes): what it is, and its length. */
static Parsed parse_ohp(const uint8_t *s, size_t n, size_t *len)
{
    if (n < 8)
        return P_NEED_MORE;
    if (s[0] == 0x16 || s[0] == 0x06) { /* check, acknowledgement: 6 + checksum */
        *len = 8;
        if (!checksum_ok(s, 6))
            return P_BAD;
        return s[0] == 0x16 && !memcmp(s + 1, "CAL00", 5) ? P_CHECK : P_OTHER;
    }
    if (s[0] != 0x0B || memcmp(s + 1, "TYP", 3))
        return P_BAD;
    if (!memcmp(s + 1, "TYP01", 5)) { /* fx-9860G, 128x64 monochrome: not shown */
        *len = 6 + 1024 + 2;
        return n < *len ? P_NEED_MORE : P_OTHER;
    }
    int fl_digits = !memcmp(s + 1, "TYPZ1", 5) ? 6 : !memcmp(s + 1, "TYPZ2", 5) ? 8 : 0;
    if (!fl_digits)
        return P_BAD;
    size_t head = 6 + (size_t)fl_digits + 12;
    if (n < head)
        return P_NEED_MORE;
    long fl = hex(s + 6, fl_digits), h = hex(s + 6 + fl_digits, 4), w = hex(s + 10 + fl_digits, 4);
    const char *ff = (const char *)s + 14 + fl_digits;
    if (fl < 0 || h <= 0 || w <= 0 || w > PANEL_WIDTH || h > PANEL_HEIGHT || picture_size(ff, w, h) != fl)
        return P_BAD;
    *len = head + (size_t)fl + 2;
    if (n < *len)
        return P_NEED_MORE;
    if (!checksum_ok(s, head + (size_t)fl))
        return P_BAD;
    decode(s + head, (int)w, (int)h, ff);
    return P_FRAME;
}

/*---------------------------------------------------------------------------
 * fxlink messages (gint add-ins)
 *-------------------------------------------------------------------------*/

#define FXLINK_HEADER  44 /* version, size, transfer size, application[16], type[16] */
#define FXLINK_VERSION 0x00000100
#define FXLINK_MAX     (STREAM_CAP - FXLINK_HEADER)

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* The message at the start of s (n bytes): P_NEED_MORE, P_BAD (not a
 * message start), or P_OTHER / P_FRAME with its length. */
static Parsed parse_fxlink(const uint8_t *s, size_t n, size_t *len)
{
    if (n < FXLINK_HEADER)
        return P_NEED_MORE;
    uint32_t size = le32(s + 4);
    if (le32(s) != FXLINK_VERSION || size > FXLINK_MAX)
        return P_BAD;
    *len = FXLINK_HEADER + size;
    if (n < *len)
        return P_NEED_MORE;
    char app[17] = {0}, type[17] = {0};
    memcpy(app, s + 12, 16);
    memcpy(type, s + 28, 16);
    const uint8_t *d = s + FXLINK_HEADER;
    if (!strcmp(app, "fxlink") && (!strcmp(type, "video") || !strcmp(type, "image")) && size >= 12) {
        long w = le32(d), h = le32(d + 4);
        uint32_t format = le32(d + 8); /* 0: RGB565 big-endian; 1, 2: fx-9860G mono, grey */
        if (format == 0 && w > 0 && h > 0 && w <= PANEL_WIDTH && h <= PANEL_HEIGHT && size - 12 >= (uint32_t)(w * h * 2)) {
            decode(d + 12, (int)w, (int)h, "1RC2");
            return P_FRAME;
        }
        LOG("  %s %ldx%ld in format %u: not shown", type, w, h, format);
    } else if (!strcmp(app, "fxlink") && !strcmp(type, "text")) {
        LOG("  text: %.*s", (int)(size > 200 ? 200 : size), (const char *)d);
    } else {
        LOG("  message %s/%s, %u bytes: ignored", app, type, size);
    }
    return P_OTHER;
}

/*---------------------------------------------------------------------------
 * USB (Linux)
 *-------------------------------------------------------------------------*/

#ifdef __linux__

typedef enum { LINK_NONE, LINK_OHP, LINK_FXLINK } Link;

typedef struct {
    int fd;
    unsigned ifno, ep_in, ep_out;
    uint32_t tag;
} Dev;

/* What a connection ended with: look again for a calculator, or give up
 * (ended_why says why). */
typedef enum { AGAIN, GIVE_UP } Outcome;

static bool read_sys(const char *dir, const char *name, char *out, size_t cap)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *f = fopen(path, "r");
    if (!f)
        return false;
    bool ok = fgets(out, (int)cap, f) != NULL;
    fclose(f);
    out[strcspn(out, "\n")] = '\0';
    return ok;
}

/* A device's interface that sends a screen: its folder, number and link. */
static Link find_interface(const char *dir, char *ifdir, size_t cap, unsigned *ifno)
{
    const char *name = strrchr(dir, '/') + 1;
    for (unsigned i = 0; i < 4; i++) {
        char cls[8];
        snprintf(ifdir, cap, "%s/%s:1.%u", dir, name, i);
        if (!read_sys(ifdir, "bInterfaceClass", cls, sizeof cls))
            continue;
        *ifno = i;
        unsigned c = (unsigned)strtoul(cls, NULL, 16);
        if (c == 0x08)
            return LINK_OHP;
        if (c == 0xFF)
            return LINK_FXLINK;
    }
    return LINK_NONE;
}

/* A Casio calculator on USB: its sysfs folder and product id. Prefers one
 * that isn't in USB Flash mode. */
static bool find_device(char *dir, size_t cap, unsigned *pid)
{
    DIR *d = opendir("/sys/bus/usb/devices");
    if (!d)
        return false;
    bool found = false;
    struct dirent *e;
    while ((e = readdir(d))) {
        char sub[400], v[16], p[16];
        if (strchr(e->d_name, ':') || e->d_name[0] == '.')
            continue; /* interfaces, not devices */
        snprintf(sub, sizeof sub, "/sys/bus/usb/devices/%s", e->d_name);
        if (!read_sys(sub, "idVendor", v, sizeof v) || strtoul(v, NULL, 16) != CASIO_VENDOR ||
            !read_sys(sub, "idProduct", p, sizeof p))
            continue;
        unsigned id = (unsigned)strtoul(p, NULL, 16);
        if (!found || *pid == USB_FLASH_PID) {
            snprintf(dir, cap, "%s", sub);
            *pid = id;
            found = true;
        }
    }
    closedir(d);
    return found;
}

/* An interface's bulk endpoints, from sysfs (ep_XX folders). */
static bool find_endpoints(const char *ifdir, Dev *dev)
{
    DIR *d = opendir(ifdir);
    if (!d)
        return false;
    dev->ep_in = dev->ep_out = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        char ep[1024], type[16], dirn[16];
        if (strncmp(e->d_name, "ep_", 3))
            continue;
        snprintf(ep, sizeof ep, "%s/%s", ifdir, e->d_name);
        if (!read_sys(ep, "type", type, sizeof type) || strcmp(type, "Bulk") || !read_sys(ep, "direction", dirn, sizeof dirn))
            continue;
        unsigned addr = (unsigned)strtoul(e->d_name + 3, NULL, 16);
        if (!strcmp(dirn, "in"))
            dev->ep_in = addr;
        else
            dev->ep_out = addr;
    }
    closedir(d);
    return dev->ep_in && dev->ep_out;
}

static int bulk(Dev *dev, unsigned ep, void *buf, unsigned len, unsigned timeout)
{
    struct usbdevfs_bulktransfer b = {.ep = ep, .len = len, .timeout = timeout, .data = buf};
    int n = ioctl(dev->fd, USBDEVFS_BULK, &b);
    if (n < 0 && errno == EPIPE) { /* stalled: clear it, the caller goes on */
        unsigned e = ep;
        ioctl(dev->fd, USBDEVFS_CLEAR_HALT, &e);
    }
    return n;
}

/* One SCSI command over Bulk-Only Transport, with a 16-byte CDB. in: the
 * data phase reads. Returns the bytes moved, or -1 (errno set). */
static int scsi(Dev *dev, const uint8_t cdb[16], bool in, void *data, unsigned len)
{
    uint8_t cbw[31] = {'U', 'S', 'B', 'C'};
    uint32_t tag = ++dev->tag;
    for (int i = 0; i < 4; i++) {
        cbw[4 + i] = (uint8_t)(tag >> 8 * i);
        cbw[8 + i] = (uint8_t)(len >> 8 * i);
    }
    cbw[12] = in ? 0x80 : 0;
    cbw[14] = 16;
    memcpy(cbw + 15, cdb, 16);
    if (bulk(dev, dev->ep_out, cbw, sizeof cbw, USB_TIMEOUT) != (int)sizeof cbw)
        return -1;
    int n = 0;
    if (len && (n = bulk(dev, in ? dev->ep_in : dev->ep_out, data, len, USB_TIMEOUT)) < 0) {
        if (errno != EPIPE)
            return -1;
        n = 0; /* stalled data phase: the status still comes */
    }
    uint8_t csw[13];
    int c = bulk(dev, dev->ep_in, csw, sizeof csw, USB_TIMEOUT);
    if (c < 0 && errno == EPIPE)
        c = bulk(dev, dev->ep_in, csw, sizeof csw, USB_TIMEOUT);
    if (c != (int)sizeof csw || memcmp(csw, "USBS", 4)) {
        if (c >= 0)
            errno = EPROTO;
        return -1;
    }
    return n;
}

static void cdb_with_length(uint8_t cdb[16], uint8_t op, unsigned len)
{
    memset(cdb, 0, 16);
    cdb[0] = op;
    cdb[6] = (uint8_t)(len >> 8);
    cdb[7] = (uint8_t)len;
}

static bool send_ack(Dev *dev)
{
    static uint8_t ACK[8] = {0x06, '0', '2', '0', '0', '1', '0', 'D'};
    uint8_t cdb[16];
    cdb_with_length(cdb, 0xC2, sizeof ACK);
    return scsi(dev, cdb, false, ACK, sizeof ACK) == (int)sizeof ACK;
}

/* The bytes received and not used yet. */
static uint8_t stream_buf[STREAM_CAP];
static size_t stream_have;

static void stream_add(const uint8_t *p, size_t n)
{
    if (stream_have + n > sizeof stream_buf)
        stream_have = 0; /* garbage piled up: start over */
    memcpy(stream_buf + stream_have, p, n);
    stream_have += n;
}

/* Uses the complete packets / messages in the stream; true unless an
 * acknowledgement couldn't be sent. */
static bool stream_use(Dev *dev, Link link)
{
    size_t at = 0;
    for (;;) {
        size_t len = 0;
        Parsed p = link == LINK_OHP ? parse_ohp(stream_buf + at, stream_have - at, &len)
                                    : parse_fxlink(stream_buf + at, stream_have - at, &len);
        if (p == P_NEED_MORE)
            break;
        if (p == P_BAD) { /* out of step: skip to what could be the next start */
            size_t i = at + 1;
            if (link == LINK_OHP)
                while (i < stream_have && stream_buf[i] != 0x0B && stream_buf[i] != 0x16 && stream_buf[i] != 0x06)
                    i++;
            else
                while (i < stream_have && stream_buf[i] != 0x00)
                    i++;
            at = i;
            continue;
        }
        if (p == P_CHECK) {
            if (!send_ack(dev)) {
                LOG("acknowledging failed: %s", strerror(errno));
                return false;
            }
            tell("Calculator connected: waiting for its screen");
        }
        at += len;
    }
    memmove(stream_buf, stream_buf + at, stream_have - at);
    stream_have -= at;
    return true;
}

/* Screen Receiver / Projector: polls with C0, reads with C1. */
static void stream_ohp(Dev *dev)
{
    static uint8_t chunk[0x10000];
    while (!SDL_GetAtomicInt(&stop)) {
        uint8_t cdb[16], st[16];
        cdb_with_length(cdb, 0xC0, 0);
        if (scsi(dev, cdb, true, st, sizeof st) < 0) {
            LOG("polling failed: %s", strerror(errno));
            return;
        }
        unsigned avail = (unsigned)(st[6] << 8 | st[7]);
        if (!avail) {
            SDL_Delay(POLL_IDLE_MS);
            continue;
        }
        cdb_with_length(cdb, 0xC1, avail);
        int n = scsi(dev, cdb, true, chunk, avail);
        if (n < 0) {
            LOG("reading failed: %s", strerror(errno));
            return;
        }
        stream_add(chunk, (size_t)n);
        if (!stream_use(dev, LINK_OHP))
            return;
    }
}

/* A gint add-in: reads its bulk endpoint. */
static void stream_fxlink(Dev *dev)
{
    static uint8_t chunk[FXLINK_CHUNK];
    tell("gint add-in connected: waiting for its screen (usb_fxlink_videocapture)");
    while (!SDL_GetAtomicInt(&stop)) {
        int n = bulk(dev, dev->ep_in, chunk, sizeof chunk, READ_TIMEOUT);
        if (n < 0 && errno == ETIMEDOUT)
            continue;
        if (n < 0 && errno != EPIPE) {
            LOG("reading failed: %s", strerror(errno));
            return;
        }
        if (n > 0) {
            stream_add(chunk, (size_t)n);
            stream_use(dev, LINK_FXLINK);
        }
    }
}

/* Finds a calculator and shows its screen until it goes away. */
static Outcome connect_once(void)
{
    char dir[400], ifdir[512], dev_path[64], v[16];
    unsigned pid = 0;
    if (!find_device(dir, sizeof dir, &pid)) {
        tell("Waiting for the calculator: Screen Receiver mode, or a gint add-in with USB open");
        return AGAIN;
    }
    if (pid == USB_FLASH_PID) {
        tell("The calculator is in USB Flash mode: plug it in again and choose Screen Receiver");
        return AGAIN;
    }
    Dev dev = {.fd = -1};
    Link link = find_interface(dir, ifdir, sizeof ifdir, &dev.ifno);
    if (link == LINK_NONE || !find_endpoints(ifdir, &dev)) {
        char msg[100];
        snprintf(msg, sizeof msg, "The calculator (07cf:%04x) isn't sending its screen", pid);
        tell(msg);
        return AGAIN;
    }
    if (!read_sys(dir, "busnum", v, sizeof v))
        return AGAIN;
    unsigned bus = (unsigned)atoi(v);
    if (!read_sys(dir, "devnum", v, sizeof v))
        return AGAIN;
    snprintf(dev_path, sizeof dev_path, "/dev/bus/usb/%03u/%03u", bus, (unsigned)atoi(v));
    LOG("%s: 07cf:%04x, %s on interface %u (endpoints %02x in, %02x out)", dev_path, pid,
        link == LINK_OHP ? "Screen Receiver" : "fxlink", dev.ifno, dev.ep_in, dev.ep_out);
    dev.fd = open(dev_path, O_RDWR);
    if (dev.fd < 0) {
        int err = errno;
        LOG("can't open %s: %s", dev_path, strerror(err));
        if (err == ENOENT || err == ENODEV)
            return AGAIN; /* went away meanwhile */
        if (err == EACCES)
            snprintf(ended_why, sizeof ended_why,
                     "No permission to use the calculator's USB device (%s). Allow it with a udev rule, "
                     "then plug the calculator in again:\n\n"
                     "echo 'SUBSYSTEM==\"usb\", ATTRS{idVendor}==\"07cf\", ATTRS{idProduct}==\"%04x\", "
                     "MODE=\"0660\", GROUP=\"plugdev\"' | sudo tee /etc/udev/rules.d/51-casio-screen.rules\n"
                     "sudo udevadm control --reload",
                     dev_path, pid);
        else
            snprintf(ended_why, sizeof ended_why, "Can't open %s: %s", dev_path, strerror(err));
        return GIVE_UP;
    }
    /* Take the interface from usb-storage (none if no driver is bound). */
    struct usbdevfs_ioctl detach = {.ifno = (int)dev.ifno, .ioctl_code = USBDEVFS_DISCONNECT};
    if (ioctl(dev.fd, USBDEVFS_IOCTL, &detach) < 0 && errno != ENODATA)
        LOG("  detaching the kernel's driver: %s", strerror(errno));
    Outcome out = AGAIN;
    if (ioctl(dev.fd, USBDEVFS_CLAIMINTERFACE, &dev.ifno) < 0) {
        int err = errno;
        if (err == ENODEV) {
            out = AGAIN;
        } else {
            snprintf(ended_why, sizeof ended_why,
                     "Can't use the calculator's USB interface: %s%s", strerror(err),
                     err == EBUSY ? " (another program, such as fxlink, is using it)" : "");
            out = GIVE_UP;
        }
    } else {
        frames = 0;
        stream_have = 0;
        if (link == LINK_OHP)
            stream_ohp(&dev);
        else
            stream_fxlink(&dev);
        LOG("connection ended after %u frame%s", frames, frames == 1 ? "" : "s");
        ioctl(dev.fd, USBDEVFS_RELEASEINTERFACE, &dev.ifno);
    }
    struct usbdevfs_ioctl attach = {.ifno = (int)dev.ifno, .ioctl_code = USBDEVFS_CONNECT};
    ioctl(dev.fd, USBDEVFS_IOCTL, &attach); /* back to usb-storage */
    close(dev.fd);
    return out;
}

static int run(void *unused)
{
    (void)unused;
    while (!SDL_GetAtomicInt(&stop) && connect_once() == AGAIN)
        for (int t = 0; t < RETRY_MS && !SDL_GetAtomicInt(&stop); t += 50)
            SDL_Delay(50);
    LOG("stopped%s%s", ended_why[0] ? ": " : "", ended_why);
    SDL_SetAtomicInt(&state, R_ENDED);
    wake();
    return 0;
}

#endif /* __linux__ */

/*---------------------------------------------------------------------------
 * The window's side
 *-------------------------------------------------------------------------*/

bool receiver_active(void)
{
    return thread != NULL;
}

/* Waits for the thread, gives the window the add-in's screen back. */
static void finish(void)
{
    SDL_WaitThread(thread, NULL);
    thread = NULL;
    SDL_SetAtomicInt(&state, R_OFF);
    view_paused = was_paused;
    view_show_frame(view_emulated_frame());
}

void receiver_start(void)
{
#ifdef __linux__
    if (thread)
        return;
    if (!lock && !(lock = SDL_CreateMutex()))
        return;
    if (!wake_event)
        wake_event = SDL_RegisterEvents(1);
    ended_why[0] = told[0] = '\0';
    SDL_SetAtomicInt(&stop, 0);
    SDL_SetAtomicInt(&frame_new, 0);
    SDL_SetAtomicInt(&note_new, 0);
    SDL_SetAtomicInt(&state, R_RUNNING);
    thread = SDL_CreateThread(run, "screen receiver", NULL);
    if (!thread) {
        SDL_SetAtomicInt(&state, R_OFF);
        return;
    }
    was_paused = view_paused;
    view_paused = true; /* the add-in waits while the calculator is shown */
#else
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, "Show calculator's screen",
                             "Showing a calculator's screen only works on Linux for now.", view_window);
#endif
}

void receiver_stop(void)
{
    if (!thread)
        return;
    SDL_SetAtomicInt(&stop, 1);
    finish();
    view_flash_status("Stopped showing the calculator's screen");
}

void receiver_poll(void)
{
    if (!thread)
        return;
    if (SDL_GetAtomicInt(&frame_new)) {
        static uint16_t shown[PANEL_WIDTH * PANEL_HEIGHT];
        SDL_LockMutex(lock);
        memcpy(shown, panel, sizeof shown);
        SDL_SetAtomicInt(&frame_new, 0);
        SDL_UnlockMutex(lock);
        view_show_frame(shown);
    }
    if (SDL_GetAtomicInt(&note_new)) {
        char msg[sizeof note];
        SDL_LockMutex(lock);
        snprintf(msg, sizeof msg, "%s", note);
        SDL_SetAtomicInt(&note_new, 0);
        SDL_UnlockMutex(lock);
        view_flash_status(msg);
    }
    if (SDL_GetAtomicInt(&state) == R_ENDED) {
        char why[sizeof ended_why];
        snprintf(why, sizeof why, "%s", ended_why);
        finish();
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, "Show calculator's screen",
                                 why[0] ? why : "Stopped showing the calculator's screen.", view_window);
    }
}
