#define _XOPEN_SOURCE 700

#include <poll.h>
#include <stdio.h>
#include <fcntl.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdbool.h>
#include <libudev.h>
#include <linux/input.h>
#include <sys/timerfd.h>
#include <wayland-client.h>

#include "idle-inhibit-unstable-v1-client-protocol.h"

#define JOYSTICKS_MAX 5
#define JOYSTICKS_FD_START 3
#define FDS_MAX JOYSTICKS_FD_START + JOYSTICKS_MAX
static struct pollfd fds[FDS_MAX];

static bool paused = false;
static int timeout_sec = 30;

struct wl_display *wl_display = NULL;
struct wl_compositor *wl_compositor = NULL;
struct wl_surface *wl_surface = NULL;
struct zwp_idle_inhibit_manager_v1 *idle_inhibit = NULL;
struct zwp_idle_inhibitor_v1 *idle_inhibitor = NULL;

static void add_device(struct udev_device *dev)
{
    const char *path = udev_device_get_syspath(dev);
    char *end = strrchr(path, '/');
    if (!end || strncmp(end, "/event", 6)) {
        return;
    }

    const char *joystick = udev_device_get_property_value(dev, "ID_INPUT_JOYSTICK");
    if (!joystick || strcmp(joystick, "1")) {
        return;
    }

    char inputdev[128];
    strcpy(inputdev, "/dev/input");
    strcat(inputdev, end);

    for (int i = 0; i < JOYSTICKS_MAX; ++i) {
        int f = JOYSTICKS_FD_START + i;
        if (fds[f].fd >= 0) {
            continue;
        }
        printf("Adding joystick %s\n", inputdev);
        int fd = open(inputdev, O_RDONLY | O_NONBLOCK);
        if (fd < 0) {
            perror("open");
            return;
        }
        printf("%s => [%d]\n", inputdev, fd);
        fds[f].fd = fd;
        fds[f].events = POLLIN | POLLERR | POLLHUP;
        return;
    }

    fprintf(stderr, "Reached limit of joysticks\n");
}

static void udev_event(struct udev_device *dev)
{
    if (!dev) {
        return;
    }
    if (!strcmp(udev_device_get_action(dev), "add")) {
        add_device(dev);
    }
    udev_device_unref(dev);
}

static void timer_event(void)
{
    uint64_t expirations;
    read(fds[1].fd, &expirations, sizeof(uint64_t));
    if (idle_inhibitor) {
        zwp_idle_inhibitor_v1_destroy(idle_inhibitor);
        idle_inhibitor = NULL;
        wl_display_flush(wl_display);
    }
}

static bool accept_event(int fd, struct input_event *ev)
{
    if (ev->type == EV_KEY || ev->type == EV_REL) {
        return true;
    }
    if (ev->type == EV_ABS) {
        // Triggers
        if (ev->code == ABS_Z || ev->code == ABS_RZ) {
            return true;
        }
        // D-Pad
        if (ev->code == ABS_HAT0X || ev->code == ABS_HAT0Y) {
            return true;
        }
        // Analog Sticks
        if (ev->code == ABS_X || ev->code == ABS_Y || ev->code == ABS_RX || ev->code == ABS_RY) {
            struct input_absinfo info;
            if (ioctl(fd, EVIOCGABS(ev->code), &info)) {
                return false;
            }
            int fuzz = (info.maximum - info.minimum) / 4;
            return ev->value < info.minimum + fuzz || ev->value > info.maximum - fuzz;
        }
    }
    return false;
}

static void device_event(int dev)
{
    if (fds[dev].revents & (POLLERR | POLLHUP)) {
        printf("Disconnected [%d]\n", fds[dev].fd);
        close(fds[dev].fd);
        fds[dev].fd = -1;
        return;
    }
    int ret = 0;
    bool accepted = false;
    struct input_event ev[128];
    while ((ret = read(fds[dev].fd, ev, sizeof(ev))) > 0) {
        if (accepted || paused) {
            continue;
        }
        for (size_t i = 0; i < ret / sizeof(struct input_event); ++i) {
            if (!accept_event(fds[dev].fd, &ev[i])) {
                continue;
            }
            struct itimerspec timeout = {0};
            timeout.it_value.tv_sec = timeout_sec;
            timerfd_settime(fds[1].fd, 0, &timeout, NULL);
            if (!idle_inhibitor) {
                idle_inhibitor = zwp_idle_inhibit_manager_v1_create_inhibitor(idle_inhibit, wl_surface);
                wl_display_flush(wl_display);
            }
            accepted = true;
            break;
        }
    }
}

static void handle_global(void *data, struct wl_registry *registry,
    uint32_t name, const char *interface, uint32_t version)
{
    (void)data, (void)version;

    if (!strcmp(interface, zwp_idle_inhibit_manager_v1_interface.name)) {
        idle_inhibit = wl_registry_bind(registry, name, &zwp_idle_inhibit_manager_v1_interface, 1);
    } else if (!strcmp(interface, wl_compositor_interface.name)) {
        wl_compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 1);
    }
}

static void handle_global_remove(void *data, struct wl_registry *registry,
    uint32_t name)
{
    (void)data, (void)registry, (void)name;
}

static const struct wl_registry_listener registry_listener = {
    .global = handle_global,
    .global_remove = handle_global_remove,
};

static void print_help(void)
{
    printf("Usage: wljoywake [options]\n");
    printf("\n");
    printf("Options:\n");
    printf("  -t TIMEOUT_SECONDS    Set idle inhibit timeout\n");
    printf("  -h --help             Show this help message\n");
    printf("  -v --version          Show version\n");
}

static void print_version(void)
{
    printf("%s\n", WLJOYWAKE_VERSION);
}

static void sig_handler(int sig)
{
    switch (sig) {
    case SIGUSR1:
        paused = true;
        break;
    case SIGUSR2:
        paused = false;
        break;
    default:
        break;
    }
}

int main(int argc, char *argv[])
{
    struct sigaction sig;
    sig.sa_handler = sig_handler;
    sigemptyset(&sig.sa_mask);
    sig.sa_flags = 0;
    sigaction(SIGUSR1, &sig, NULL);
    sigaction(SIGUSR2, &sig, NULL);

    for (int i = 0; i < FDS_MAX; ++i) {
        fds[i].fd = -1;
    }

    if (argc > 1) {
        if (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help")) {
            print_help();
            return 0;
        } else if (!strcmp(argv[1], "-v") || !strcmp(argv[1], "--version")) {
            print_version();
            return 0;
        } else if (!strcmp(argv[1], "-t")) {
            if (argc < 3) {
                print_help();
                return 1;
            }
            timeout_sec = atoi(argv[2]);
        }
    }

    // udev
    struct udev *u = udev_new();
    struct udev_enumerate *enumerate = udev_enumerate_new(u);
    udev_enumerate_add_match_subsystem(enumerate, "input");
    udev_enumerate_scan_devices(enumerate);
    struct udev_list_entry *devices = udev_enumerate_get_list_entry(enumerate);
    struct udev_list_entry *dev_list_entry;
    udev_list_entry_foreach(dev_list_entry, devices) {
        const char *path = udev_list_entry_get_name(dev_list_entry);
        struct udev_device *dev = udev_device_new_from_syspath(u, path);
        add_device(dev);
        udev_device_unref(dev);
    }
    udev_enumerate_unref(enumerate);

    struct udev_monitor *monitor = udev_monitor_new_from_netlink(u, "udev");
    udev_monitor_filter_add_match_subsystem_devtype(monitor, "input", NULL);
    udev_monitor_enable_receiving(monitor);

    fds[0].fd = udev_monitor_get_fd(monitor);
    fds[0].events = POLLIN;

    // timer
    fds[1].fd = timerfd_create(CLOCK_REALTIME, TFD_NONBLOCK);
    fds[1].events = POLLIN;

    // wayland
    wl_display = wl_display_connect(NULL);
    if (!wl_display) {
        fprintf(stderr, "Failed to open Wayland display!\n");
        return 2;
    }
    struct wl_registry *registry = wl_display_get_registry(wl_display);
    wl_registry_add_listener(registry, &registry_listener, NULL);
    wl_display_roundtrip(wl_display);
    if (!idle_inhibit) {
        fprintf(stderr, "zwp_idle_inhibit_manager_v1 not available!\n");
        return 2;
    }
    wl_surface = wl_compositor_create_surface(wl_compositor);
    wl_display_flush(wl_display);

    fds[2].fd = wl_display_get_fd(wl_display);
    fds[2].events = POLLIN;

    while (1) {
        int ret = poll(fds, FDS_MAX, -1);
        if (ret <= 0) {
            continue;
        }
        for (int i = 0; i < FDS_MAX; ++i) {
            if (!fds[i].revents) {
                continue;
            }
            if (i == 0) {
                udev_event(udev_monitor_receive_device(monitor));
            } else if (i == 1) {
                timer_event();
            } else if (i == 2) {
                if (wl_display_dispatch(wl_display) == -1) {
                    goto quit;
                }
            } else {
                device_event(i);
            }
        }
    }
quit:
    wl_display_disconnect(wl_display);

    return 0;
}
