# Wayland Joystick Wake

Wayland tool for idle inhibit when using joysticks. Requires idle_inhibit_unstable_v1 protocol.

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
