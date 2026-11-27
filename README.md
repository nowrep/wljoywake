# Wayland Joystick Wake

Wayland tool for idle inhibit when using joysticks. Requires idle_inhibit_unstable_v1 protocol.

Compositors such as sway (1.11 and newer) and river ignore inhibitors on surfaces
that are not mapped. If the compositor supports wlr_layer_shell_unstable_v1, the
inhibitor is attached to a transparent 1x1 layer surface.

    Usage: wljoywake [options]

    Options:
      -t TIMEOUT_SECONDS    Set idle wakeup timeout
      -h --help             Show this help message
      -v --version          Show version


### Dependencies

* wayland-client
* wayland-scanner
* wayland-protocols
* libudev

### Building

    meson setup --prefix /usr build
    ninja -C build && ninja -C build install
