/*
 * Copyright (c) 2009-2018 Vadim Mikhailov
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

/* Max number of hub ports supported.
 * This is somewhat artifically limited by "-p" option parser.
 * If "-p" parser is improved, we can support up to 32 ports.
 * However, biggest number of ports on smart hub I've seen was 8.
 * I've also observed onboard USB hub with whopping 14 ports,
 * but that hub did not support per-port power switching.
 */
#define MAX_HUB_PORTS            9
#define ALL_HUB_PORTS            ((1 << MAX_HUB_PORTS) - 1) /* bitmask */

#include "usb.h"

#define POWER_KEEP               (-1)
#define POWER_OFF                0
#define POWER_ON                 1
#define POWER_CYCLE              2

/* default options */
static char opt_vendor[VENDOR_LEN_MAX]   = "";
static char opt_location[LOCATION_LEN_MAX] = "";     /* Hub location a-b.c.d */
static int opt_ports  = ALL_HUB_PORTS; /* Bitmask of ports to operate on */
static int opt_action = POWER_KEEP;
static int opt_delay  = 2;
static int opt_repeat = 1;
static int opt_wait   = 20; /* wait before repeating in ms */
static int opt_exact  = 0;  /* exact location match - disable USB3 duality handling */
static int opt_reset  = 0;  /* reset hub after operation(s) */

static const struct option long_options[] = {
    { "loc",      required_argument, NULL, 'l' },
    { "vendor",   required_argument, NULL, 'n' },
    { "ports",    required_argument, NULL, 'p' },
    { "action",   required_argument, NULL, 'a' },
    { "delay",    required_argument, NULL, 'd' },
    { "repeat",   required_argument, NULL, 'r' },
    { "wait",     required_argument, NULL, 'w' },
    { "exact",    no_argument,       NULL, 'e' },
    { "reset",    no_argument,       NULL, 'R' },
    { "version",  no_argument,       NULL, 'v' },
    { "help",     no_argument,       NULL, 'h' },
    { 0,          0,                 NULL, 0   },
};


static int print_usage()
{
    printf(
        "uhubctl %s: utility to control USB port power for smart hubs.\n"
        "Usage: uhubctl [options]\n"
        "Without options, show status for all smart hubs.\n"
        "\n"
        "Options [defaults in brackets]:\n"
        "--action,   -a - action to off/on/cycle (0/1/2) for affected ports.\n"
        "--ports,    -p - ports to operate on    [all hub ports].\n"
        "--loc,      -l - limit hub by location  [all smart hubs].\n"
        "--vendor,   -n - limit hub by vendor id [%s] (partial ok).\n"
        "--delay,    -d - delay for cycle action [%d sec].\n"
        "--repeat,   -r - repeat power off count [%d] (some devices need it to turn off).\n"
        "--exact,    -e - exact location (no USB3 duality handling).\n"
        "--reset,    -R - reset hub after each power-on action, causing all devices to reassociate.\n"
        "--wait,     -w - wait before repeat power off [%d ms].\n"
        "--version,  -v - print program version.\n"
        "--help,     -h - print this text.\n"
        "\n"
        "Send bugs and requests to: https://github.com/mvp/uhubctl\n",
        PROGRAM_VERSION,
        strlen(opt_vendor) ? opt_vendor : "any",
        opt_delay,
        opt_repeat,
        opt_wait
    );
    return 0;
}


int main(int argc, char *argv[])
{
    int rc;
    int c = 0;
    int option_index = 0;

    for (;;) {
        c = getopt_long(argc, argv, "l:n:a:p:d:r:w:hveR",
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
            if (!strcasecmp(optarg, "all")) { /* all ports is the default */
                break;
            }
            if (strlen(optarg)) {
                /* parse port list */
                opt_ports = 0;
                size_t i;
                for (i=0; i<strlen(optarg); i++) {
                    if (!isdigit(optarg[i]) || optarg[i] == '0') {
                        printf("%s must be list of ports 1 to %d\n", optarg, MAX_HUB_PORTS);
                    }
                    int d = optarg[i]-'1';
                    opt_ports |= (1 << d);
                }
            }
            break;
        case 'a':
            if (!strcasecmp(optarg, "off")   || !strcasecmp(optarg, "0")) {
                opt_action = POWER_OFF;
            }
            if (!strcasecmp(optarg, "on")    || !strcasecmp(optarg, "1")) {
                opt_action = POWER_ON;
            }
            if (!strcasecmp(optarg, "cycle") || !strcasecmp(optarg, "2")) {
                opt_action = POWER_CYCLE;
            }
            break;
        case 'd':
            opt_delay = atoi(optarg);
            break;
        case 'r':
            opt_repeat = atoi(optarg);
            break;
        case 'e':
            opt_exact = 1;
            break;
        case 'R':
            opt_reset = 1;
            break;
        case 'w':
            opt_wait = atoi(optarg);
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

    if (hub_phys_count > 1 && opt_action >= 0) {
        fprintf(stderr,
            "Error: changing port state for multiple hubs at once is not supported.\n"
            "Use -l to limit operation to one hub!\n"
        );
        exit(1);
    }
    int k; /* k=0 for power OFF, k=1 for power ON */
    for (k=0; k<2; k++) { /* up to 2 power actions - off/on */
        if (k == 0 && opt_action == POWER_ON )
            continue;
        if (k == 1 && opt_action == POWER_OFF)
            continue;
        if (k == 1 && opt_action == POWER_KEEP)
            continue;
        int i;
        for (i=0; i<hub_count; i++) {
            if (hubs[i].actionable == 0)
                continue;
            printf("Current status for hub %s [%s]\n",
                hubs[i].location, hubs[i].description
            );
            print_port_status(&hubs[i], opt_ports);
            if (opt_action == POWER_KEEP) { /* no action, show status */
                continue;
            }
            struct libusb_device_handle * devh = NULL;
            rc = libusb_open(hubs[i].dev, &devh);
            if (rc == 0) {
                /* will operate on these ports */
                int ports = ((1 << hubs[i].nports) - 1) & opt_ports;
                int request = (k == 0) ? LIBUSB_REQUEST_CLEAR_FEATURE
                                       : LIBUSB_REQUEST_SET_FEATURE;
                int port;
                for (port=1; port <= hubs[i].nports; port++) {
                    if ((1 << (port-1)) & ports) {
                        int port_status = get_port_status(devh, port);
                        int power_mask = hubs[i].bcd_usb < USB_SS_BCD ? USB_PORT_STAT_POWER
                                                                          : USB_SS_PORT_STAT_POWER;
                        if (k == 0 && !(port_status & power_mask))
                            continue;
                        if (k == 1 && (port_status & power_mask))
                            continue;
                        int repeat = 1;
                        if (k == 0)
                            repeat = opt_repeat;
                        if (!(port_status & ~power_mask))
                            repeat = 1;
                        while (repeat-- > 0) {
                            rc = libusb_control_transfer(devh,
                                LIBUSB_REQUEST_TYPE_CLASS | LIBUSB_RECIPIENT_OTHER,
                                request, USB_PORT_FEAT_POWER,
                                port, NULL, 0, USB_CTRL_GET_TIMEOUT
                            );
                            if (rc < 0) {
                                perror("Failed to control port power!\n");
                            }
                            if (repeat > 0) {
                                sleep_ms(opt_wait);
                            }
                        }
                    }
                }
                /* USB3 hubs need extra delay to actually turn off: */
                if (k==0 && hubs[i].bcd_usb >= USB_SS_BCD)
                    sleep_ms(150);
                printf("Sent power %s request\n",
                    request == LIBUSB_REQUEST_CLEAR_FEATURE ? "off" : "on"
                );
                printf("New status for hub %s [%s]\n",
                    hubs[i].location, hubs[i].description
                );
                print_port_status(&hubs[i], opt_ports);

                if (k == 1 && opt_reset == 1) {
                    printf("Resetting hub...\n");
                    rc = libusb_reset_device(devh);
                    if (rc < 0) {
                        perror("Reset failed!\n");
                    } else {
                        printf("Reset successful!\n");
                    }
                }
            }
            libusb_close(devh);
        }
        if (k == 0 && opt_action == POWER_CYCLE)
            sleep_ms(opt_delay * 1000);
    }
    rc = 0;
cleanup:
    if (usb_devs)
        libusb_free_device_list(usb_devs, 1);
    usb_devs = NULL;
    libusb_exit(NULL);
    return rc;
}
