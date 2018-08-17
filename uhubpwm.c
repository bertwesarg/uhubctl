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

#define _XOPEN_SOURCE 500

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <getopt.h>
#include <errno.h>
#include <ctype.h>

#if defined(_WIN32)
#include <windows.h>
#include <io.h>
#include <process.h>
#define strcasecmp _stricmp
#define strncasecmp _strnicmp
#else
#include <unistd.h>
#endif

#include "usb.h"

/* default options */
static char opt_vendor[VENDOR_LEN_MAX]   = "";
static char opt_location[LOCATION_LEN_MAX] = "";     /* Hub location a-b.c.d */
static int opt_port  = -1;
static double opt_delay = .5;
static int opt_exact  = 0;  /* exact location match - disable USB3 duality handling */

static const struct option long_options[] = {
    { "loc",      required_argument, NULL, 'l' },
    { "vendor",   required_argument, NULL, 'n' },
    { "port",     required_argument, NULL, 'p' },
    { "delay",    required_argument, NULL, 'd' },
    { "exact",    no_argument,       NULL, 'e' },
    { "version",  no_argument,       NULL, 'v' },
    { "help",     no_argument,       NULL, 'h' },
    { 0,          0,                 NULL, 0   },
};


static int print_usage()
{
    printf(
        "uhubpwm %s: PWM of USB port power.\n"
        "Usage: uhubctl [options]\n"
        "\n"
        "Options [defaults in brackets]:\n"
        "--port,     -p - port to operate on.\n"
        "--loc,      -l - select hub by location.\n"
        "--vendor,   -n - select hub by vendor id (partial ok).\n"
        "--delay,    -d - delay for PWM [%.0f sec].\n"
        "--exact,    -e - exact location (no USB3 duality handling).\n"
        "--version,  -v - print program version.\n"
        "--help,     -h - print this text.\n"
        "\n"
        "Send bugs and requests to: https://github.com/mvp/uhubctl\n",
        PROGRAM_VERSION,
        opt_delay
    );
    return 0;
}


int main(int argc, char *argv[])
{
    int rc;
    int c = 0;
    int option_index = 0;

    for (;;) {
        c = getopt_long(argc, argv, "l:n:p:d:hve",
            long_options, &option_index);
        if (c == -1)
            break;  /* no more options left */
        switch (c) {
        case 0:
            /* If this option set a flag, do nothing else now. */
            if (long_options[option_index].flag != 0)
                break;
            printf("option %s", long_options[option_index].name);
            if (optarg)
                printf(" with arg %s", optarg);
            printf("\n");
            break;
        case 'l':
            strncpy(opt_location, optarg, sizeof(opt_location));
            break;
        case 'n':
            strncpy(opt_vendor, optarg, sizeof(opt_vendor));
            break;
        case 'p':
            if (strlen(optarg)) {
                /* parse port */
                opt_port = atoi(optarg) - 1;
            }
            break;
        case 'd':
            opt_delay = atof(optarg);
            break;
        case 'e':
            opt_exact = 1;
            break;
        case 'v':
            printf("%s\n", PROGRAM_VERSION);
            exit(0);
            break;
        case 'h':
            print_usage();
            exit(1);
            break;
        case '?':
            /* getopt_long has already printed an error message here */
            fprintf(stderr, "Run with -h to get usage info.\n");
            exit(1);
            break;
        default:
            abort();
        }
    }
    if (optind < argc) {
        /* non-option parameters are found? */
        fprintf(stderr, "Invalid command line syntax!\n");
        fprintf(stderr, "Run with -h to get usage info.\n");
        exit(1);
    }

    rc = libusb_init(NULL);
    if (rc < 0) {
        fprintf(stderr,
            "Error initializing USB!\n"
        );
        exit(1);
    }

    rc = libusb_get_device_list(NULL, &usb_devs);
    if (rc < 0) {
        fprintf(stderr,
            "Cannot enumerate USB devices!\n"
        );
        rc = 1;
        goto cleanup;
    }

    rc = usb_find_hubs(opt_location, opt_vendor, opt_exact);
    if (rc <= 0) {
        fprintf(stderr,
            "No compatible smart hubs detected%s%s!\n"
            "Run with -h to get usage info.\n",
            strlen(opt_location) ? " at location " : "",
            opt_location
        );
#ifdef __gnu_linux__
        if (rc < 0) {
            fprintf(stderr,
                "There were permission problems while accessing USB.\n"
                "To fix this, run this tool as root using 'sudo uhubctl',\n"
                "or add one or more udev rules like below\n"
                "to file '/etc/udev/rules.d/52-usb.rules':\n"
                "SUBSYSTEM==\"usb\", ATTR{idVendor}==\"2001\", MODE=\"0666\"\n"
                "then run 'sudo udevadm trigger --attr-match=subsystem=usb'\n"
            );
        }
#endif
        rc = 1;
        goto cleanup;
    }

    if (hub_phys_count != 1) {
        perror("Need exactly one hub!\n");
    }

    int i;
    struct hub_info * hub = NULL;
    for (i=0; i<hub_count; i++) {
        /* Check only actionable USB3 hubs: */
        if (!hubs[i].actionable)
            continue;
        hub = &hubs[i];
        break;
    }
    if (hub == NULL) {
        perror("No hub selected!\n");
    }

    if (opt_port < 0) {
        perror("Need exactly one port!\n");
    }

    if (opt_port >= hub->nports) {
        perror("Port out of range!\n");
    }

    struct libusb_device_handle * devh = NULL;
    rc = libusb_open(hub->dev, &devh);
    if (rc != 0) {
        libusb_close(devh);
        exit(1);
    }

    rc = libusb_control_transfer(devh,
        LIBUSB_REQUEST_TYPE_CLASS | LIBUSB_RECIPIENT_OTHER,
        LIBUSB_REQUEST_CLEAR_FEATURE, USB_PORT_FEAT_POWER,
        opt_port, NULL, 0, USB_CTRL_GET_TIMEOUT
    );
    if (rc < 0) {
        perror("Failed to control port power!\n");
    }

    int k = 0;
    while (1) {
        int request = (k & 1) ? LIBUSB_REQUEST_SET_FEATURE
                              : LIBUSB_REQUEST_CLEAR_FEATURE;
        k++;

        rc = libusb_control_transfer(devh,
            LIBUSB_REQUEST_TYPE_CLASS | LIBUSB_RECIPIENT_OTHER,
            request, USB_PORT_FEAT_POWER,
            opt_port, NULL, 0, USB_CTRL_GET_TIMEOUT
        );
        if (rc < 0) {
            perror("Failed to control port power!\n");
        }
        if (request == LIBUSB_REQUEST_SET_FEATURE)
            sleep_ms(opt_delay * 1000);
        else
            sleep_ms(10);
    }

    rc = libusb_control_transfer(devh,
        LIBUSB_REQUEST_TYPE_CLASS | LIBUSB_RECIPIENT_OTHER,
        LIBUSB_REQUEST_SET_FEATURE, USB_PORT_FEAT_POWER,
        opt_port, NULL, 0, USB_CTRL_GET_TIMEOUT
    );
    if (rc < 0) {
        perror("Failed to control port power!\n");
    }

    libusb_close(devh);

    rc = 0;
cleanup:
    if (usb_devs)
        libusb_free_device_list(usb_devs, 1);
    usb_devs = NULL;
    libusb_exit(NULL);
    return rc;
}
