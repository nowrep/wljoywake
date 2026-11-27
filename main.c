#define _GNU_SOURCE

#include <poll.h>
#include <stdio.h>
#include <fcntl.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdbool.h>
#include <libudev.h>
#include <sys/mman.h>
#include <linux/input.h>
#include <sys/timerfd.h>
#include <wayland-client.h>

#include "idle-inhibit-unstable-v1-client-protocol.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

#define JOYSTICKS_MAX 5
#define JOYSTICKS_FD_START 3
#define FDS_MAX JOYSTICKS_FD_START + JOYSTICKS_MAX
static struct pollfd fds[FDS_MAX];

static bool paused = false;
static bool active = false;
static bool surface_ready = false;
static int timeout_sec = 30;

struct wl_display *wl_display = NULL;
struct wl_compositor *wl_compositor = NULL;
struct wl_shm *wl_shm = NULL;
struct wl_buffer *wl_buffer = NULL;
struct wl_surface *wl_surface = NULL;
struct zwlr_layer_shell_v1 *layer_shell = NULL;
struct zwlr_layer_surface_v1 *layer_surface = NULL;
struct zwp_idle_inhibit_manager_v1 *idle_inhibit = NULL;
struct zwp_idle_inhibitor_v1 *idle_inhibitor = NULL;

static void create_inhibitor(void)
{
    if (!idle_inhibitor && active && surface_ready) {
        idle_inhibitor = zwp_idle_inhibit_manager_v1_create_inhibitor(idle_inhibit, wl_surface);
    }
}

static void destroy_inhibitor(void)
{
    if (idle_inhibitor) {
        zwp_idle_inhibitor_v1_destroy(idle_inhibitor);
        idle_inhibitor = NULL;
    }
}

static void destroy_surface(void)
{
    destroy_inhibitor();
    if (layer_surface) {
        zwlr_layer_surface_v1_destroy(layer_surface);
        layer_surface = NULL;
    }
    if (wl_surface) {
        wl_surface_destroy(wl_surface);
        wl_surface = NULL;
    }
    surface_ready = false;
}

static void layer_surface_configure(void *data, struct zwlr_layer_surface_v1 *surface,
    uint32_t serial, uint32_t width, uint32_t height)
{
    (void)data, (void)width, (void)height;

    zwlr_layer_surface_v1_ack_configure(surface, serial);
    wl_surface_attach(wl_surface, wl_buffer, 0, 0);
    wl_surface_commit(wl_surface);
    surface_ready = true;
    create_inhibitor();
    wl_display_flush(wl_display);
}

static void layer_surface_closed(void *data, struct zwlr_layer_surface_v1 *surface)
{
    (void)data, (void)surface;

    // Recreated on the next accepted joystick event, not here, so a missing
    // output cannot cause a create/closed loop
    destroy_surface();
    wl_display_flush(wl_display);
}

static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
    .configure = layer_surface_configure,
    .closed = layer_surface_closed,
};

static struct wl_buffer *create_buffer(void)
{
    // Zero-filled, a single transparent ARGB pixel
    int fd = memfd_create("wljoywake", MFD_CLOEXEC);
    if (fd < 0) {
        perror("memfd_create");
        return NULL;
    }
    if (ftruncate(fd, 4) < 0) {
        perror("ftruncate");
        close(fd);
        return NULL;
    }
    struct wl_shm_pool *pool = wl_shm_create_pool(wl_shm, fd, 4);
    struct wl_buffer *buffer = wl_shm_pool_create_buffer(pool, 0, 1, 1, 4, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    return buffer;
}

// Some compositors ignore inhibitors on surfaces that are not mapped, and sway
// only re-checks when an inhibitor is created or destroyed, so the inhibitor
// waits for the first configure. A bare surface is the fallback.
static void create_surface(void)
{
    if (wl_surface) {
        return;
    }
    wl_surface = wl_compositor_create_surface(wl_compositor);
    if (!wl_buffer) {
        surface_ready = true;
        return;
    }
    struct wl_region *region = wl_compositor_create_region(wl_compositor);
    wl_surface_set_input_region(wl_surface, region);
    wl_region_destroy(region);
    layer_surface = zwlr_layer_shell_v1_get_layer_surface(layer_shell, wl_surface, NULL,
        ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND, "wljoywake");
    zwlr_layer_surface_v1_add_listener(layer_surface, &layer_surface_listener, NULL);
    zwlr_layer_surface_v1_set_size(layer_surface, 1, 1);
    zwlr_layer_surface_v1_set_anchor(layer_surface,
        ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT);
    wl_surface_commit(wl_surface);
}

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
    active = false;
    destroy_inhibitor();
    wl_display_flush(wl_display);
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
            active = true;
            create_surface();
            create_inhibitor();
            wl_display_flush(wl_display);
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
    } else if (!strcmp(interface, wl_shm_interface.name)) {
        wl_shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    } else if (!strcmp(interface, zwlr_layer_shell_v1_interface.name)) {
        layer_shell = wl_registry_bind(registry, name, &zwlr_layer_shell_v1_interface, 1);
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
    if (layer_shell && wl_shm) {
        wl_buffer = create_buffer();
    }
    create_surface();
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
