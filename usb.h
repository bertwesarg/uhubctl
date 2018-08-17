/*
 * Copyright (c) 2009-2018 Vadim Mikhailov
 * Copyright (c) 2018 Bert Wesarg
 *
 * Utility to turn USB port power on/off
 * for USB hubs that support per-port power switching.
 *
 * This file can be distributed under the terms and conditions of the
 * GNU General Public License version 2.
 *
 */

#if defined(__FreeBSD__) || defined(_WIN32)
#include <libusb.h>
#else
#include <libusb-1.0/libusb.h>
#endif

#if _POSIX_C_SOURCE >= 199309L
#include <time.h>   /* for nanosleep */
#endif

#if defined(__APPLE__) /* snprintf is not available in pure C mode */
int snprintf(char * __restrict __str, size_t __size, const char * __restrict __format, ...) __printflike(3, 4);
#endif

#define USB_CTRL_GET_TIMEOUT     5000

#define MAX_HUB_CHAIN            8  /* Per USB 3.0 spec max hub chain is 7 */

#define VENDOR_LEN_MAX   16
#define LOCATION_LEN_MAX 32


/* cross-platform sleep function */

static void sleep_ms(int milliseconds)
{
#if defined(_WIN32)
    Sleep(milliseconds);
#elif _POSIX_C_SOURCE >= 199309L
    struct timespec ts;
    ts.tv_sec = milliseconds / 1000;
    ts.tv_nsec = (milliseconds % 1000) * 1000000;
    nanosleep(&ts, NULL);
#else
    usleep(milliseconds * 1000);
#endif
}

/* Partially borrowed from linux/usb/ch11.h */

#pragma pack(push,1)
struct usb_hub_descriptor {
    unsigned char bDescLength;
    unsigned char bDescriptorType;
    unsigned char bNbrPorts;
    unsigned char wHubCharacteristics[2];
    unsigned char bPwrOn2PwrGood;
    unsigned char bHubContrCurrent;
    unsigned char data[1]; /* use 1 to avoid zero-sized array warning */
};
#pragma pack(pop)

/*
 * Hub Status and Hub Change results
 * See USB 2.0 spec Table 11-19 and Table 11-20
 */
#pragma pack(push,1)
struct usb_port_status {
    int16_t wPortStatus;
    int16_t wPortChange;
};
#pragma pack(pop)

#define USB_PORT_FEAT_POWER      (1 << 3)

/*
 * wPortStatus bit field
 * See USB 2.0 spec Table 11-21
 */
#define USB_PORT_STAT_CONNECTION        0x0001
#define USB_PORT_STAT_ENABLE            0x0002
#define USB_PORT_STAT_SUSPEND           0x0004
#define USB_PORT_STAT_OVERCURRENT       0x0008
#define USB_PORT_STAT_RESET             0x0010
#define USB_PORT_STAT_L1                0x0020
/* bits 6 to 7 are reserved */
#define USB_PORT_STAT_POWER             0x0100
#define USB_PORT_STAT_LOW_SPEED         0x0200
#define USB_PORT_STAT_HIGH_SPEED        0x0400
#define USB_PORT_STAT_TEST              0x0800
#define USB_PORT_STAT_INDICATOR         0x1000
/* bits 13 to 15 are reserved */


#define USB_SS_BCD                      0x0300
/*
 * Additions to wPortStatus bit field from USB 3.0
 * See USB 3.0 spec Table 10-10
 */
#define USB_PORT_STAT_LINK_STATE        0x01e0
#define USB_SS_PORT_STAT_POWER          0x0200
#define USB_SS_PORT_STAT_SPEED          0x1c00
#define USB_PORT_STAT_SPEED_5GBPS       0x0000
/* Valid only if port is enabled */
/* Bits that are the same from USB 2.0 */
#define USB_SS_PORT_STAT_MASK (USB_PORT_STAT_CONNECTION  | \
                               USB_PORT_STAT_ENABLE      | \
                               USB_PORT_STAT_OVERCURRENT | \
                               USB_PORT_STAT_RESET)

/*
 * Definitions for PORT_LINK_STATE values
 * (bits 5-8) in wPortStatus
 */
#define USB_SS_PORT_LS_U0               0x0000
#define USB_SS_PORT_LS_U1               0x0020
#define USB_SS_PORT_LS_U2               0x0040
#define USB_SS_PORT_LS_U3               0x0060
#define USB_SS_PORT_LS_SS_DISABLED      0x0080
#define USB_SS_PORT_LS_RX_DETECT        0x00a0
#define USB_SS_PORT_LS_SS_INACTIVE      0x00c0
#define USB_SS_PORT_LS_POLLING          0x00e0
#define USB_SS_PORT_LS_RECOVERY         0x0100
#define USB_SS_PORT_LS_HOT_RESET        0x0120
#define USB_SS_PORT_LS_COMP_MOD         0x0140
#define USB_SS_PORT_LS_LOOPBACK         0x0160


/*
 * wHubCharacteristics (masks)
 * See USB 2.0 spec Table 11-13, offset 3
 */
#define HUB_CHAR_LPSM           0x0003 /* Logical Power Switching Mode mask */
#define HUB_CHAR_COMMON_LPSM    0x0000 /* All ports at once power switching */
#define HUB_CHAR_INDV_PORT_LPSM 0x0001 /* Per-port power switching */
#define HUB_CHAR_NO_LPSM        0x0002 /* No power switching */

#define HUB_CHAR_COMPOUND       0x0004 /* hub is part of a compound device */

#define HUB_CHAR_OCPM           0x0018 /* Over-Current Protection Mode mask */
#define HUB_CHAR_COMMON_OCPM    0x0000 /* All ports at once over-current protection */
#define HUB_CHAR_INDV_PORT_OCPM 0x0008 /* Per-port over-current protection */
#define HUB_CHAR_NO_OCPM        0x0010 /* No over-current protection support */

#define HUB_CHAR_TTTT           0x0060 /* TT Think Time mask */
#define HUB_CHAR_PORTIND        0x0080 /* per-port indicators (LEDs) */

/* List of all USB devices enumerated by libusb */
static struct libusb_device **usb_devs = NULL;

struct hub_info {
    struct libusb_device *dev;
    int bcd_usb;
    int nports;
    int ppps;
    int actionable; /* true if this hub is subject to action */
    char vendor[VENDOR_LEN_MAX];
    char location[LOCATION_LEN_MAX];
    char description[256];
};

/* Array of all enumerated USB hubs */
#define MAX_HUBS 128
static struct hub_info hubs[MAX_HUBS];
static int hub_count = 0;
static int hub_phys_count = 0;

/* trim trailing spaces from a string */

static char* rtrim(char* str)
{
    int i;
    for (i = strlen(str)-1; i>=0 && isspace(str[i]); i--) {
        str[i] = 0;
    }
    return str;
}

/*
 * get USB hub properties.
 * most hub_info fields are filled, except for description.
 * returns 0 for success and error code for failure.
 */

static int get_hub_info(struct libusb_device *dev, struct hub_info *info)
{
    int rc = 0;
    int len = 0;
    struct libusb_device_handle *devh = NULL;
    unsigned char buf[LIBUSB_DT_HUB_NONVAR_SIZE + 2 + 3] = {0};
    struct usb_hub_descriptor *uhd = (struct usb_hub_descriptor *)buf;
    int minlen = LIBUSB_DT_HUB_NONVAR_SIZE + 2;
    struct libusb_device_descriptor desc;
    rc = libusb_get_device_descriptor(dev, &desc);
    if (rc)
        return rc;
    if (desc.bDeviceClass != LIBUSB_CLASS_HUB)
        return LIBUSB_ERROR_INVALID_PARAM;
    int bcd_usb = libusb_le16_to_cpu(desc.bcdUSB);
    int desc_type = bcd_usb >= USB_SS_BCD ? LIBUSB_DT_SUPERSPEED_HUB
                                          : LIBUSB_DT_HUB;
    rc = libusb_open(dev, &devh);
    if (rc == 0) {
        len = libusb_control_transfer(devh,
            LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_CLASS
                               | LIBUSB_RECIPIENT_DEVICE, /* hub status */
            LIBUSB_REQUEST_GET_DESCRIPTOR,
            desc_type << 8,
            0,
            buf, sizeof(buf),
            USB_CTRL_GET_TIMEOUT
        );

        if (len >= minlen) {
            unsigned char port_numbers[MAX_HUB_CHAIN] = {0};
            info->dev     = dev;
            info->bcd_usb = bcd_usb;
            info->nports  = uhd->bNbrPorts;
            snprintf(
                info->vendor, sizeof(info->vendor),
                "%04x:%04x",
                libusb_le16_to_cpu(desc.idVendor),
                libusb_le16_to_cpu(desc.idProduct)
            );

            /* Convert bus and ports array into USB location string */
            int bus = libusb_get_bus_number(dev);
            snprintf(info->location, sizeof(info->location), "%d", bus);
#if defined(LIBUSB_API_VERSION) && (LIBUSB_API_VERSION >= 0x01000102)
            /*
             * libusb_get_port_path is deprecated since libusb v1.0.16,
             * therefore use libusb_get_port_numbers when supported
             */
            int pcount = libusb_get_port_numbers(dev, port_numbers, MAX_HUB_CHAIN);
#else
            int pcount = libusb_get_port_path(NULL, dev, port_numbers, MAX_HUB_CHAIN);
#endif
            int k;
            for (k=0; k<pcount; k++) {
                char s[8];
                snprintf(s, sizeof(s), "%s%d", k==0 ? "-" : ".", port_numbers[k]);
                strcat(info->location, s);
            }

            info->ppps = 0;
            /* Logical Power Switching Mode */
            int lpsm = uhd->wHubCharacteristics[0] & HUB_CHAR_LPSM;
            /* Over-Current Protection Mode */
            int ocpm = uhd->wHubCharacteristics[0] & HUB_CHAR_OCPM;
            /* LPSM must be supported per-port, and OCPM per port or ganged */
            if ((lpsm == HUB_CHAR_INDV_PORT_LPSM) &&
                (ocpm == HUB_CHAR_INDV_PORT_OCPM ||
                 ocpm == HUB_CHAR_COMMON_OCPM))
            {
                info->ppps = 1;
            }
        } else {
            rc = len;
        }
        libusb_close(devh);
    }
    return rc;
}


/*
 * Assuming that devh is opened device handle for USB hub,
 * return state for given hub port.
 * In case of error, returns -1 (inspect errno for more information).
 */

static int get_port_status(struct libusb_device_handle *devh, int port)
{
    int rc;
    struct usb_port_status ust;
    if (devh == NULL)
        return -1;

    rc = libusb_control_transfer(devh,
        LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_CLASS
                           | LIBUSB_RECIPIENT_OTHER, /* port status */
        LIBUSB_REQUEST_GET_STATUS, 0,
        port, (unsigned char*)&ust, sizeof(ust),
        USB_CTRL_GET_TIMEOUT
    );

    if (rc < 0) {
        return rc;
    }
    return ust.wPortStatus;
}


/*
 * Get USB device description as a string.
 *
 * It will use following format:
 *
 *    "<vid:pid> <vendor> <product> <serial>, <USB x.yz, N ports>"
 *
 * vid:pid will be always present, but vendor, product or serial
 * may be skipped if they are empty or not enough permissions to read them.
 * <USB x.yz, N ports> will be present only for USB hubs.
 *
 * returns 0 for success and error code for failure.
 * in case of failure description buffer is not altered.
 */

static int get_device_description(struct libusb_device * dev, char* description, int desc_len)
{
    int rc;
    int id_vendor  = 0;
    int id_product = 0;
    char vendor[64]  = "";
    char product[64] = "";
    char serial[64]  = "";
    char ports[64]   = "";
    struct libusb_device_descriptor desc;
    struct libusb_device_handle *devh = NULL;
    rc = libusb_get_device_descriptor(dev, &desc);
    if (rc)
        return rc;
    id_vendor  = libusb_le16_to_cpu(desc.idVendor);
    id_product = libusb_le16_to_cpu(desc.idProduct);
    rc = libusb_open(dev, &devh);
    if (rc == 0) {
        if (desc.iManufacturer) {
            libusb_get_string_descriptor_ascii(devh,
                desc.iManufacturer, (unsigned char*)vendor, sizeof(vendor));
            rtrim(vendor);
        }
        if (desc.iProduct) {
            libusb_get_string_descriptor_ascii(devh,
                desc.iProduct, (unsigned char*)product, sizeof(product));
            rtrim(product);
        }
        if (desc.iSerialNumber) {
            libusb_get_string_descriptor_ascii(devh,
                desc.iSerialNumber, (unsigned char*)serial, sizeof(serial));
            rtrim(serial);
        }
        if (desc.bDeviceClass == LIBUSB_CLASS_HUB) {
            struct hub_info info;
            rc = get_hub_info(dev, &info);
            if (rc == 0) {
                snprintf(ports, sizeof(ports), ", USB %x.%02x, %d ports",
                   info.bcd_usb >> 8, info.bcd_usb & 0xFF, info.nports);
            }
        }
        libusb_close(devh);
    }
    snprintf(description, desc_len,
        "%04x:%04x%s%s%s%s%s%s%s",
        id_vendor, id_product,
        vendor[0]  ? " " : "", vendor,
        product[0] ? " " : "", product,
        serial[0]  ? " " : "", serial,
        ports
    );
    return 0;
}


/*
 * show status for hub ports
 * portmask is bitmap of ports to display
 * if portmask is 0, show all ports
 */

static int print_port_status(struct hub_info * hub, int portmask)
{
    int port_status;
    struct libusb_device_handle * devh = NULL;
    int rc = 0;
    struct libusb_device *dev = hub->dev;
    rc = libusb_open(dev, &devh);
    if (rc == 0) {
        int port;
        for (port = 1; port <= hub->nports; port++) {
            if (portmask > 0 && (portmask & (1 << (port-1))) == 0) continue;

            port_status = get_port_status(devh, port);
            if (port_status == -1) {
                fprintf(stderr,
                    "cannot read port %d status, %s (%d)\n",
                    port, strerror(errno), errno);
                break;
            }

            printf("  Port %d: %04x", port, port_status);

            char description[256] = "";
            struct libusb_device * udev;
            int i = 0;
            while ((udev = usb_devs[i++]) != NULL) {
                if (libusb_get_parent(udev)      == dev &&
                    libusb_get_port_number(udev) == port)
                {
                    rc = get_device_description(udev, description, sizeof(description));
                    if (rc == 0)
                        break;
                }
            }

            if (hub->bcd_usb < USB_SS_BCD) {
                if (port_status == 0) {
                    printf(" off");
                } else {
                    if (port_status & USB_PORT_STAT_POWER)        printf(" power");
                    if (port_status & USB_PORT_STAT_INDICATOR)    printf(" indicator");
                    if (port_status & USB_PORT_STAT_TEST)         printf(" test");
                    if (port_status & USB_PORT_STAT_HIGH_SPEED)   printf(" highspeed");
                    if (port_status & USB_PORT_STAT_LOW_SPEED)    printf(" lowspeed");
                    if (port_status & USB_PORT_STAT_SUSPEND)      printf(" suspend");
                }
            } else {
                if (port_status == USB_SS_PORT_LS_SS_DISABLED) {
                    printf(" off");
                } else {
                    int link_state = port_status & USB_PORT_STAT_LINK_STATE;
                    if (port_status & USB_SS_PORT_STAT_POWER)     printf(" power");
                    if ((port_status & USB_SS_PORT_STAT_SPEED)
                         == USB_PORT_STAT_SPEED_5GBPS)
                    {
                        printf(" 5gbps");
                    }
                    if (link_state == USB_SS_PORT_LS_U0)          printf(" U0");
                    if (link_state == USB_SS_PORT_LS_U1)          printf(" U1");
                    if (link_state == USB_SS_PORT_LS_U2)          printf(" U2");
                    if (link_state == USB_SS_PORT_LS_U3)          printf(" U3");
                    if (link_state == USB_SS_PORT_LS_SS_DISABLED) printf(" SS.Disabled");
                    if (link_state == USB_SS_PORT_LS_RX_DETECT)   printf(" Rx.Detect");
                    if (link_state == USB_SS_PORT_LS_SS_INACTIVE) printf(" SS.Inactive");
                    if (link_state == USB_SS_PORT_LS_POLLING)     printf(" Polling");
                    if (link_state == USB_SS_PORT_LS_RECOVERY)    printf(" Recovery");
                    if (link_state == USB_SS_PORT_LS_HOT_RESET)   printf(" HotReset");
                    if (link_state == USB_SS_PORT_LS_COMP_MOD)    printf(" Compliance");
                    if (link_state == USB_SS_PORT_LS_LOOPBACK)    printf(" Loopback");
                }
            }
            if (port_status & USB_PORT_STAT_RESET)       printf(" reset");
            if (port_status & USB_PORT_STAT_OVERCURRENT) printf(" oc");
            if (port_status & USB_PORT_STAT_ENABLE)      printf(" enable");
            if (port_status & USB_PORT_STAT_CONNECTION)  printf(" connect");

            if (port_status & USB_PORT_STAT_CONNECTION)  printf(" [%s]", description);

            printf("\n");
        }
        libusb_close(devh);
    }
    return 0;
}


/*
 *  Find all USB hubs and fill hubs[] array.
 *  Set actionable to 1 on all hubs that we are going to operate on
 *  (this applies possible constraints like location or vendor).
 *  Returns count of found actionable physical hubs
 *  (USB3 hubs are counted once despite having USB2 dual partner).
 *  In case of error returns negative error code.
 */

static int usb_find_hubs(const char* location, const char* vendor, int exact)
{
    struct libusb_device *dev;
    int perm_ok = 1;
    int rc = 0;
    int i = 0;
    int j = 0;
    while ((dev = usb_devs[i++]) != NULL) {
        struct libusb_device_descriptor desc;
        rc = libusb_get_device_descriptor(dev, &desc);
        /* only scan for hubs: */
        if (rc == 0 && desc.bDeviceClass != LIBUSB_CLASS_HUB)
            continue;
        struct hub_info info;
        bzero(&info, sizeof(info));
        rc = get_hub_info(dev, &info);
        if (rc) {
            perm_ok = 0; /* USB permission issue? */
        }
        get_device_description(dev, info.description, sizeof(info.description));
        if (info.ppps) { /* PPPS is supported */
            if (hub_count < MAX_HUBS) {
                info.actionable = 1;
                if (strlen(location)>0) {
                    if (strcasecmp(location, info.location)) {
                       info.actionable = 0;
                    }
                }
                if (strlen(vendor)>0) {
                    if (strncasecmp(vendor, info.vendor, strlen(vendor))) {
                        info.actionable = 0;
                    }
                }
                memcpy(&hubs[hub_count], &info, sizeof(info));
                hub_count++;
            }
        }
    }
    hub_phys_count = 0;
    for (i=0; i<hub_count; i++) {
        /* Check only actionable USB3 hubs: */
        if (!hubs[i].actionable)
            continue;
        if (hubs[i].bcd_usb < USB_SS_BCD || exact) {
            hub_phys_count++;
        }
        if (exact)
            continue;
        int match = -1;
        for (j=0; j<hub_count; j++) {
            if (i==j)
                continue;

            /* Find hub which is USB2/3 dual to the hub above.
             * This is quite reliable and predictable on Linux
             * but not on Mac, where we may match wrong hub :(
             * It will work reliably on Mac if there is
             * only one compatible USB3 hub is connected.
             * TODO: discover better way to find dual hub.
             */

            /* Hub and its dual must be different types: one USB2, another USB3: */
            if ((hubs[i].bcd_usb < USB_SS_BCD) ==
                (hubs[j].bcd_usb < USB_SS_BCD))
                continue;

            /* But they must have the same vendor: */
            if (strncasecmp(hubs[i].vendor, hubs[j].vendor, 4))
                continue;

            /* Provisionally we choose this one as dual: */
            if (match < 0 && !hubs[j].actionable)
                match = j;

            /* But if there is exact port path match,
             * we prefer it (true for Linux but not Mac):
             */
            char *p1 = strchr(hubs[i].location, '-');
            char *p2 = strchr(hubs[j].location, '-');
            if (p1 && p2 && strcasecmp(p1, p2)==0) {
                match = j;
                break;
            }
        }
        if (match >= 0)
            hubs[match].actionable = 1;
    }
    if (perm_ok == 0 && hub_phys_count == 0) {
        return LIBUSB_ERROR_ACCESS;
    }
    return hub_phys_count;
}


