/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2025 Marcin Szewczyk-Wilgan <contact@m4c.pl>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
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
 *
 * sndchk - Real-time audio diagnostics for FreeBSD
 *
 * Monitors audio buffer xruns, USB transfer errors, and IRQ spikes.
 *
 * Usage:
 *   sndchk                     List devices and show help
 *   sndchk -w                  Monitor default device
 *   sndchk -d N -w             Monitor device pcmN
 *   sndchk -xruns -w           Monitor only xruns
 *   sndchk -usb -w             Monitor only USB errors and IRQ
 *
 * Build:
 *   cc -o sndchk sndchk.c
 *
 * License: BSD-2-Clause
 */

#include <sys/types.h>
#include <sys/sysctl.h>
#include <sys/ioctl.h>
#include <sys/soundcard.h>

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <signal.h>
#include <errno.h>
#include <regex.h>

#define MAX_DEVICES 16
#define MAX_CHANNELS 8
#define MAX_LINE 1024
#define IRQ_CALIBRATION_SAMPLES 10

/* Global flag for signal handling */
static volatile sig_atomic_t running = 1;

/* Configuration */
struct config {
    int device;
    int play_only;
    int show_xruns;
    int show_usb;
    int watch_mode;
    int interval;
    float irq_threshold;
};

/* Device info */
struct pcm_device {
    int unit;
    char desc[256];
    char parent[64];
    int is_usb;
    char ugen[16];      /* e.g., "0.4" */
    char controller[16]; /* e.g., "xhci0" */
    char irq[16];       /* e.g., "irq64" */
    int is_default;
};

/* Channel xruns */
struct channel_xruns {
    char name[64];      /* e.g., "pcm6.play.0" */
    int xruns;
};

/* USB stats */
struct usb_stats {
    int ctrl_fail;
    int iso_fail;
    int bulk_fail;
    int int_fail;
};

/* Signal handler */
static void
sigint_handler(int sig __unused)
{
    running = 0;
}

/* Get current timestamp as string */
static void
get_timestamp(char *buf, size_t len)
{
    time_t now;
    struct tm *tm_info;

    time(&now);
    tm_info = localtime(&now);
    strftime(buf, len, "%H:%M:%S", tm_info);
}

/* Execute command and capture output */
static int
exec_cmd(const char *cmd, char *output, size_t outlen)
{
    FILE *fp;
    size_t total = 0;
    char buf[256];

    output[0] = '\0';
    
    fp = popen(cmd, "r");
    if (fp == NULL)
        return -1;

    while (fgets(buf, sizeof(buf), fp) != NULL) {
        size_t len = strlen(buf);
        if (total + len < outlen - 1) {
            strcat(output, buf);
            total += len;
        }
    }

    pclose(fp);
    return 0;
}

/* Get sysctl string value */
static int
sysctl_get_string(const char *name, char *buf, size_t len)
{
    size_t size = len;
    
    if (sysctlbyname(name, buf, &size, NULL, 0) < 0)
        return -1;
    
    buf[size] = '\0';
    return 0;
}

/* Get sysctl integer value */
static int
sysctl_get_int(const char *name)
{
    int val;
    size_t size = sizeof(val);
    
    if (sysctlbyname(name, &val, &size, NULL, 0) < 0)
        return -1;
    
    return val;
}

/* Get default audio unit */
static int
get_default_unit(void)
{
    int unit = sysctl_get_int("hw.snd.default_unit");
    return (unit >= 0) ? unit : 0;
}

/* Find ugen device for pcm unit */
static int
find_usb_for_pcm(int unit, char *ugen, size_t ugen_len)
{
    char sysctl_name[64];
    char parent[64];
    char location[256];
    char *p;
    int uaudio_num;

    /* Get parent (e.g., "uaudio0") */
    snprintf(sysctl_name, sizeof(sysctl_name), "dev.pcm.%d.%%parent", unit);
    if (sysctl_get_string(sysctl_name, parent, sizeof(parent)) < 0)
        return -1;

    /* Check if it's uaudio */
    if (strncmp(parent, "uaudio", 6) != 0)
        return -1;

    uaudio_num = atoi(parent + 6);

    /* Get location which contains ugen= */
    snprintf(sysctl_name, sizeof(sysctl_name), "dev.uaudio.%d.%%location", uaudio_num);
    if (sysctl_get_string(sysctl_name, location, sizeof(location)) < 0)
        return -1;

    /* Parse ugen=ugenX.Y from location */
    p = strstr(location, "ugen=ugen");
    if (p == NULL)
        return -1;

    p += 9; /* skip "ugen=ugen" */
    
    /* Copy until space or end */
    size_t i = 0;
    while (p[i] && p[i] != ' ' && i < ugen_len - 1) {
        ugen[i] = p[i];
        i++;
    }
    ugen[i] = '\0';

    return 0;
}

/* Find USB controller for ugen device */
static int
find_usb_controller(const char *ugen, char *controller, size_t ctrl_len,
                    char *irq, size_t irq_len)
{
    char sysctl_name[64];
    char parent[64];
    char cmd[128];
    char output[1024];
    char *line, *p;
    int bus;

    /* Extract bus number from ugen (e.g., "0.4" -> 0) */
    bus = atoi(ugen);

    /* Get parent of usbus (e.g., xhci0) */
    snprintf(sysctl_name, sizeof(sysctl_name), "dev.usbus.%d.%%parent", bus);
    if (sysctl_get_string(sysctl_name, parent, sizeof(parent)) < 0)
        return -1;

    /* Remove trailing newline if any */
    p = strchr(parent, '\n');
    if (p) *p = '\0';

    strncpy(controller, parent, ctrl_len - 1);
    controller[ctrl_len - 1] = '\0';

    /* Find IRQ in vmstat -i output */
    snprintf(cmd, sizeof(cmd), "vmstat -i | grep '%s'", controller);
    if (exec_cmd(cmd, output, sizeof(output)) < 0)
        return -1;

    /* Parse first field (e.g., "irq64:") */
    line = output;
    p = strchr(line, ':');
    if (p == NULL)
        return -1;

    *p = '\0';
    
    /* Skip leading whitespace */
    while (*line == ' ' || *line == '\t')
        line++;

    strncpy(irq, line, irq_len - 1);
    irq[irq_len - 1] = '\0';

    return 0;
}

/* Get IRQ count from vmstat -i */
static long
get_irq_count(const char *irq)
{
    char cmd[128];
    char output[256];
    char *p;
    long count = 0;

    snprintf(cmd, sizeof(cmd), "vmstat -i | grep '%s:'", irq);
    if (exec_cmd(cmd, output, sizeof(output)) < 0)
        return 0;

    /* Parse third field (total count) */
    /* Format: "irq64: xhci0    12345    100" */
    p = output;
    
    /* Skip first field (irq) */
    while (*p && *p != ' ' && *p != '\t') p++;
    while (*p == ' ' || *p == '\t') p++;
    
    /* Skip second field (controller name) */
    while (*p && *p != ' ' && *p != '\t') p++;
    while (*p == ' ' || *p == '\t') p++;
    
    /* Third field is count */
    count = strtol(p, NULL, 10);

    return count;
}

/* Get xruns for a device */
static int
get_xruns(int unit, int play_only, struct channel_xruns *channels, int max_channels)
{
    char cmd[128];
    char output[4096];
    char *line, *saveptr;
    int count = 0;

    snprintf(cmd, sizeof(cmd), "sndctl -f /dev/dsp%d -v -o 2>/dev/null", unit);
    if (exec_cmd(cmd, output, sizeof(output)) < 0)
        return 0;

    line = strtok_r(output, "\n", &saveptr);
    while (line != NULL && count < max_channels) {
        char *xruns_ptr = strstr(line, "xruns=");
        if (xruns_ptr != NULL) {
            /* Check play_only filter */
            if (play_only && strstr(line, "play") == NULL) {
                line = strtok_r(NULL, "\n", &saveptr);
                continue;
            }

            /* Extract channel name (remove .xruns=N) */
            char *dot = strstr(line, ".xruns=");
            if (dot) {
                size_t name_len = dot - line;
                if (name_len >= sizeof(channels[count].name))
                    name_len = sizeof(channels[count].name) - 1;
                
                strncpy(channels[count].name, line, name_len);
                channels[count].name[name_len] = '\0';
                
                /* Convert dsp to pcm */
                if (strncmp(channels[count].name, "dsp", 3) == 0) {
                    channels[count].name[0] = 'p';
                    channels[count].name[1] = 'c';
                    channels[count].name[2] = 'm';
                }
            }

            /* Extract xruns value */
            channels[count].xruns = atoi(xruns_ptr + 6);
            count++;
        }
        line = strtok_r(NULL, "\n", &saveptr);
    }

    return count;
}

/* Get USB stats */
static int
get_usb_stats(const char *ugen, struct usb_stats *stats)
{
    char cmd[128];
    char output[2048];
    char *line, *saveptr;

    memset(stats, 0, sizeof(*stats));

    snprintf(cmd, sizeof(cmd), "usbconfig -d %s dump_stats 2>/dev/null", ugen);
    if (exec_cmd(cmd, output, sizeof(output)) < 0)
        return -1;

    if (strlen(output) == 0)
        return -1;

    line = strtok_r(output, "\n", &saveptr);
    while (line != NULL) {
        char *p;
        
        if ((p = strstr(line, "UE_CONTROL_FAIL:")) != NULL) {
            stats->ctrl_fail = atoi(p + 16);
        } else if ((p = strstr(line, "UE_ISOCHRONOUS_FAIL:")) != NULL) {
            stats->iso_fail = atoi(p + 20);
        } else if ((p = strstr(line, "UE_BULK_FAIL:")) != NULL) {
            stats->bulk_fail = atoi(p + 13);
        } else if ((p = strstr(line, "UE_INTERRUPT_FAIL:")) != NULL) {
            stats->int_fail = atoi(p + 18);
        }

        line = strtok_r(NULL, "\n", &saveptr);
    }

    return 0;
}

/* List available audio devices */
static int
list_devices(struct pcm_device *devices, int max_devices)
{
    FILE *fp;
    char line[512];
    int count = 0;
    int default_unit = get_default_unit();

    fp = fopen("/dev/sndstat", "r");
    if (fp == NULL) {
        perror("Cannot open /dev/sndstat");
        return 0;
    }

    while (fgets(line, sizeof(line), fp) != NULL && count < max_devices) {
        if (strncmp(line, "pcm", 3) != 0)
            continue;

        struct pcm_device *dev = &devices[count];
        memset(dev, 0, sizeof(*dev));

        /* Parse unit number */
        dev->unit = atoi(line + 3);

        /* Parse description (after ": ") */
        char *desc_start = strchr(line, ':');
        if (desc_start) {
            desc_start += 2; /* skip ": " */
            char *newline = strchr(desc_start, '\n');
            if (newline) *newline = '\0';
            strncpy(dev->desc, desc_start, sizeof(dev->desc) - 1);
        }

        /* Check if default */
        dev->is_default = (dev->unit == default_unit);

        /* Check if USB and get ugen */
        if (find_usb_for_pcm(dev->unit, dev->ugen, sizeof(dev->ugen)) == 0) {
            dev->is_usb = 1;
            find_usb_controller(dev->ugen, dev->controller, sizeof(dev->controller),
                               dev->irq, sizeof(dev->irq));
        }

        count++;
    }

    fclose(fp);
    return count;
}

/* Print device list */
static void
print_devices(struct pcm_device *devices, int count)
{
    printf("Available audio devices:\n\n");

    for (int i = 0; i < count; i++) {
        struct pcm_device *dev = &devices[i];
        
        printf("  pcm%d", dev->unit);
        
        if (dev->is_default)
            printf(" (default)");
        
        if (dev->is_usb)
            printf(" [usb:%s]", dev->ugen);
        
        printf(": %s\n", dev->desc);
    }

    printf("\n");
}

/* Print usage */
static void
usage(const char *progname)
{
    printf("usage: %s [-d device] [-p] [-xruns] [-usb] [-w] [-i interval] [-t threshold]\n\n", progname);
    printf("Options:\n");
    printf("  -d N      Monitor device pcmN (default: system default)\n");
    printf("  -p        Show only playback channels\n");
    printf("  -xruns    Show only xruns (no USB errors, no IRQ monitoring)\n");
    printf("  -usb      Show only USB errors and IRQ monitoring (no xruns)\n");
    printf("  -w        Watch mode - start monitoring\n");
    printf("  -i SEC    Interval in seconds (default: 1)\n");
    printf("  -t N      IRQ spike threshold multiplier (default: 1.5)\n");
    printf("  -h        Show this help\n\n");
    printf("Notes:\n");
    printf("  Without -w, shows available devices and exits.\n");
    printf("  IRQ monitoring is enabled when USB monitoring is active.\n");
    printf("  Use -usb to monitor only USB errors and IRQ spikes.\n");
    printf("  Use -xruns to monitor only audio buffer xruns (no IRQ).\n\n");
    printf("Examples:\n");
    printf("  %s              List available audio devices\n", progname);
    printf("  %s -w           Monitor default device\n", progname);
    printf("  %s -d 1 -w      Monitor pcm1\n", progname);
    printf("  %s -d 0 -p -w   Monitor only playback xruns on pcm0\n", progname);
    printf("  %s -xruns -w    Monitor only xruns\n", progname);
    printf("  %s -usb -w      Monitor only USB errors and IRQ\n", progname);
    printf("  %s -t 2.0 -w    Set IRQ spike threshold to 2x baseline\n", progname);
}

/* Main watch loop */
static void
watch_loop(struct config *cfg, struct pcm_device *dev)
{
    char timestamp[16];
    struct channel_xruns channels[MAX_CHANNELS];
    struct channel_xruns prev_channels[MAX_CHANNELS];
    int num_channels = 0;
    int prev_num_channels = 0;
    
    struct usb_stats usb, prev_usb;
    memset(&prev_usb, 0, sizeof(prev_usb));
    
    long prev_irq_count = 0;
    long irq_baseline = 0;
    int irq_samples = 0;

    /* Print header */
    printf("Monitoring pcm%d: %s\n", dev->unit, dev->desc);
    
    if (dev->is_usb && cfg->show_usb) {
        printf("USB device: ugen%s\n", dev->ugen);
        if (dev->controller[0])
            printf("USB controller: %s (%s)\n", dev->controller, dev->irq);
    }
    
    printf("----------------------------------------\n");

    /* Initialize USB stats */
    if (dev->is_usb && cfg->show_usb) {
        get_usb_stats(dev->ugen, &prev_usb);
    }

    /* Initialize IRQ count */
    if (dev->irq[0] && cfg->show_usb) {
        prev_irq_count = get_irq_count(dev->irq);
    }

    /* Print initial values */
    get_timestamp(timestamp, sizeof(timestamp));

    if (cfg->show_xruns) {
        num_channels = get_xruns(dev->unit, cfg->play_only, channels, MAX_CHANNELS);
        printf("[%s] Initial xruns:", timestamp);
        for (int i = 0; i < num_channels; i++) {
            printf(" %s=%d", channels[i].name, channels[i].xruns);
        }
        printf("\n");
        
        memcpy(prev_channels, channels, sizeof(channels));
        prev_num_channels = num_channels;
    }

    if (cfg->show_usb && dev->is_usb) {
        printf("[%s] Initial USB: CTRL=%d ISO=%d BULK=%d INT=%d\n",
               timestamp, prev_usb.ctrl_fail, prev_usb.iso_fail,
               prev_usb.bulk_fail, prev_usb.int_fail);

        if (dev->irq[0]) {
            printf("[%s] Initial IRQ: calibrating...\n", timestamp);
        }
    }

    /* Main loop */
    while (running) {
        sleep(cfg->interval);
        
        if (!running)
            break;

        get_timestamp(timestamp, sizeof(timestamp));

        /* Check xruns */
        if (cfg->show_xruns) {
            num_channels = get_xruns(dev->unit, cfg->play_only, channels, MAX_CHANNELS);
            
            for (int i = 0; i < num_channels; i++) {
                if (channels[i].xruns == 0)
                    continue;

                /* Find previous value */
                int prev_val = 0;
                for (int j = 0; j < prev_num_channels; j++) {
                    if (strcmp(channels[i].name, prev_channels[j].name) == 0) {
                        prev_val = prev_channels[j].xruns;
                        break;
                    }
                }

                if (channels[i].xruns != prev_val) {
                    int diff = channels[i].xruns - prev_val;
                    printf("[%s] %s xruns: %d -> %d (+%d)\n",
                           timestamp, channels[i].name,
                           prev_val, channels[i].xruns, diff);
                }
            }

            memcpy(prev_channels, channels, sizeof(channels));
            prev_num_channels = num_channels;
        }

        /* Check USB errors */
        if (cfg->show_usb && dev->is_usb) {
            if (get_usb_stats(dev->ugen, &usb) < 0) {
                printf("[%s] USB WARNING: Device disconnected or not responding\n",
                       timestamp);
            } else {
                if (usb.ctrl_fail != prev_usb.ctrl_fail) {
                    int diff = usb.ctrl_fail - prev_usb.ctrl_fail;
                    printf("[%s] UE_CONTROL_FAIL: %d -> %d (+%d)\n",
                           timestamp, prev_usb.ctrl_fail, usb.ctrl_fail, diff);
                    prev_usb.ctrl_fail = usb.ctrl_fail;
                }

                if (usb.iso_fail != prev_usb.iso_fail) {
                    int diff = usb.iso_fail - prev_usb.iso_fail;
                    printf("[%s] UE_ISOCHRONOUS_FAIL: %d -> %d (+%d)\n",
                           timestamp, prev_usb.iso_fail, usb.iso_fail, diff);
                    prev_usb.iso_fail = usb.iso_fail;
                }

                if (usb.bulk_fail != prev_usb.bulk_fail) {
                    int diff = usb.bulk_fail - prev_usb.bulk_fail;
                    printf("[%s] UE_BULK_FAIL: %d -> %d (+%d)\n",
                           timestamp, prev_usb.bulk_fail, usb.bulk_fail, diff);
                    prev_usb.bulk_fail = usb.bulk_fail;
                }

                if (usb.int_fail != prev_usb.int_fail) {
                    int diff = usb.int_fail - prev_usb.int_fail;
                    printf("[%s] UE_INTERRUPT_FAIL: %d -> %d (+%d)\n",
                           timestamp, prev_usb.int_fail, usb.int_fail, diff);
                    prev_usb.int_fail = usb.int_fail;
                }
            }
        }

        /* Check IRQ rate */
        if (dev->irq[0] && cfg->show_usb) {
            long curr_irq_count = get_irq_count(dev->irq);
            long irq_rate = curr_irq_count - prev_irq_count;

            /* Build baseline over first N samples */
            if (irq_samples < IRQ_CALIBRATION_SAMPLES) {
                irq_samples++;
                irq_baseline = ((irq_baseline * (irq_samples - 1)) + irq_rate) / irq_samples;

                if (irq_samples == IRQ_CALIBRATION_SAMPLES) {
                    printf("[%s] %s baseline: %ld/s\n",
                           timestamp, dev->controller, irq_baseline);
                }
            } else {
                /* Check for spike */
                if (irq_baseline > 0) {
                    long threshold = (long)(irq_baseline * cfg->irq_threshold);
                    if (irq_rate > threshold) {
                        float ratio = (float)irq_rate / irq_baseline;
                        printf("[%s] %s: %ld -> %ld/s (%.1fx)\n",
                               timestamp, dev->controller,
                               irq_baseline, irq_rate, ratio);
                    }
                }
            }

            prev_irq_count = curr_irq_count;
        }
    }

    printf("\nMonitoring stopped.\n");
}

int
main(int argc, char *argv[])
{
    struct config cfg = {
        .device = -1,
        .play_only = 0,
        .show_xruns = 1,
        .show_usb = 1,
        .watch_mode = 0,
        .interval = 1,
        .irq_threshold = 1.5f
    };

    struct pcm_device devices[MAX_DEVICES];
    int num_devices;

    /* Parse arguments */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            cfg.device = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-i") == 0 && i + 1 < argc) {
            cfg.interval = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) {
            cfg.irq_threshold = atof(argv[++i]);
        } else if (strcmp(argv[i], "-p") == 0) {
            cfg.play_only = 1;
        } else if (strcmp(argv[i], "-w") == 0) {
            cfg.watch_mode = 1;
        } else if (strcmp(argv[i], "-xruns") == 0) {
            cfg.show_xruns = 1;
            cfg.show_usb = 0;
        } else if (strcmp(argv[i], "-usb") == 0) {
            cfg.show_xruns = 0;
            cfg.show_usb = 1;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            num_devices = list_devices(devices, MAX_DEVICES);
            print_devices(devices, num_devices);
            usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            usage(argv[0]);
            return 1;
        }
    }

    /* List devices */
    num_devices = list_devices(devices, MAX_DEVICES);

    /* If not watch mode, show devices and help */
    if (!cfg.watch_mode) {
        print_devices(devices, num_devices);
        usage(argv[0]);
        return 0;
    }

    /* Use default device if not specified */
    if (cfg.device < 0) {
        cfg.device = get_default_unit();
    }

    /* Find device in list */
    struct pcm_device *target = NULL;
    for (int i = 0; i < num_devices; i++) {
        if (devices[i].unit == cfg.device) {
            target = &devices[i];
            break;
        }
    }

    if (target == NULL) {
        fprintf(stderr, "Error: device pcm%d not found\n", cfg.device);
        return 1;
    }

    /* Check USB availability */
    if (cfg.show_usb && !target->is_usb) {
        fprintf(stderr, "Warning: Could not find USB device for pcm%d\n", cfg.device);
        fprintf(stderr, "USB monitoring disabled.\n");
        cfg.show_usb = 0;
    }

    /* Setup signal handler */
    signal(SIGINT, sigint_handler);
    signal(SIGTERM, sigint_handler);

    /* Run watch loop */
    watch_loop(&cfg, target);

    return 0;
}
