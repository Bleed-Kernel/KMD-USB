#include <usb/usb.h>
#include <usb/usb_hub.h>
#include <usb/usb_hcd.h>
#include <drivers/serial/serial.h>
#include <ansii.h>
#include <string.h>
#include <stdio.h>

typedef struct usb_hub {
    volatile bool     used;
    usb_device_t     *dev;
    bool              super;
    uint8_t           ports;
    volatile uint32_t changed;      // bit per port, the status pipe sets these
    volatile uint32_t redo;
    usb_device_t     *child[USB_HUB_MAX_PORTS + 1];
    uint8_t           resets[USB_HUB_MAX_PORTS + 1];
} usb_hub_t;

static usb_hub_t hubs[USB_HUB_MAX];

static const struct {
    uint16_t change;
    uint8_t  feature;
} port_changes[] = {
    { 1u << 0, USB_PORT_C_CONNECTION },
    { 1u << 1, USB_PORT_C_ENABLE },
    { 1u << 2, USB_PORT_C_SUSPEND },
    { 1u << 3, USB_PORT_C_OVER_CURRENT },
    { 1u << 4, USB_PORT_C_RESET },
    { 1u << 5, USB_PORT_C_BH_RESET },
    { 1u << 6, USB_PORT_C_LINK_STATE },
    { 1u << 7, USB_PORT_C_CONFIG_ERROR },
};

#define PORT_CHANGE_COUNT (sizeof(port_changes) / sizeof(port_changes[0]))

static usb_hub_t *usb_hub_of(const usb_device_t *dev) {
    for (int i = 0; i < USB_HUB_MAX; i++) {
        if (hubs[i].used && hubs[i].dev == dev)
            return &hubs[i];
    }
    return NULL;
}

static int usb_hub_port_feature(usb_hub_t *hub, uint8_t request, uint8_t port, uint8_t feature) {
    usb_setup_t setup = {
        .request_type = USB_REQ_DIR_OUT | USB_REQ_TYPE_CLASS | USB_REQ_RCPT_OTHER,
        .request      = request,
        .value        = feature,
        .index        = port,
    };
    return usb_control(hub->dev, &setup, NULL);
}

static int usb_hub_port_status(usb_hub_t *hub, uint8_t port, uint16_t *status, uint16_t *change) {
    uint16_t words[2] = { 0, 0 };
    usb_setup_t setup = {
        .request_type = USB_REQ_DIR_IN | USB_REQ_TYPE_CLASS | USB_REQ_RCPT_OTHER,
        .request      = USB_HUB_REQ_GET_STATUS,
        .index        = port,
        .length       = sizeof(words),
    };

    int r = usb_control(hub->dev, &setup, words);
    if (r < (int)sizeof(words)) {
        serial_printf(LOG_WARN "usbhub: %04x:%04x port %u status failed (%d)\n",
                      hub->dev->vendor_id, hub->dev->product_id, port, r);
        return r < 0 ? r : USB_ERR;
    }
    *status = words[0];
    *change = words[1];
    return 0;
}

static void usb_hub_status_report(void *ctx, const uint8_t *data, size_t len) {
    usb_hub_t *hub  = ctx;
    uint32_t   bits = 0;

    if (!hub->used)
        return;
    for (size_t i = 0; i < len && i < sizeof(bits); i++)
        bits |= (uint32_t)data[i] << (i * 8);
    __atomic_fetch_or(&hub->changed, bits, __ATOMIC_RELAXED);
}

static int usb_hub_port_reset(usb_hub_t *hub, uint8_t port, usb_speed_t *speed) {
    uint16_t status = 0, change = 0;

    int r = usb_hub_port_feature(hub, USB_HUB_REQ_SET, port, USB_PORT_RESET);
    if (r < 0)
        return r;

    uint32_t waited = 0;
    for (;;) {
        usb_delay_ms(USB_HUB_RESET_POLL_MS);
        waited += USB_HUB_RESET_POLL_MS;

        r = usb_hub_port_status(hub, port, &status, &change);
        if (r < 0)
            return r;
        if (change & (USB_PORT_CH_RESET | USB_PORT_CH_BH_RESET))
            break;
        if (waited >= USB_HUB_RESET_TIMEOUT_MS) {
            serial_printf(LOG_WARN "usbhub: port %u reset timed out, status 0x%04x change 0x%04x\n",
                          port, status, change);
            return USB_ERR_TIMEOUT;
        }
    }

    // a reset can drop the link for a moment, that isnt a replug
    usb_hub_port_feature(hub, USB_HUB_REQ_CLEAR, port, USB_PORT_C_RESET);
    usb_hub_port_feature(hub, USB_HUB_REQ_CLEAR, port, USB_PORT_C_BH_RESET);
    usb_hub_port_feature(hub, USB_HUB_REQ_CLEAR, port, USB_PORT_C_CONNECTION);
    usb_hub_port_feature(hub, USB_HUB_REQ_CLEAR, port, USB_PORT_C_ENABLE);
    usb_delay_ms(USB_HUB_RECOVERY_MS);

    if (!(status & USB_PORT_ST_ENABLED)) {
        serial_printf(LOG_WARN "usbhub: port %u didnt enable, status 0x%04x\n", port, status);
        return USB_ERR;
    }

    if (hub->super)
        *speed = USB_SPEED_SUPER;
    else if (status & USB_PORT_ST_LOW_SPEED)
        *speed = USB_SPEED_LOW;
    else if (status & USB_PORT_ST_HIGH_SPEED)
        *speed = USB_SPEED_HIGH;
    else
        *speed = USB_SPEED_FULL;
    return 0;
}

static void usb_hub_port_connect(usb_hub_t *hub, uint8_t port) {
    usb_device_t *dev = hub->dev;

    for (int tries = 0; tries < USB_HUB_ATTACH_TRIES && !dev->gone; tries++) {
        usb_speed_t speed;

        if (usb_hub_port_reset(hub, port, &speed) == 0) {
            serial_printf(LOG_INFO "usbhub: port %u enabled at %s speed\n", port, usb_speed_name(speed));
            hub->child[port] = dev->ops->device_add(dev, port, speed);
            if (hub->child[port])
                return;
        }

        serial_printf(LOG_WARN "usbhub: port %u attach try %d of %d failed\n",
                      port, tries + 1, USB_HUB_ATTACH_TRIES);
        usb_delay_ms(USB_HUB_RETRY_MS);
    }
    serial_printf(LOG_WARN "usbhub: giving up on port %u\n", port);
}

static void usb_hub_port_remove(usb_hub_t *hub, uint8_t port) {
    usb_device_t *child = hub->child[port];
    if (!child)
        return;

    hub->child[port] = NULL;
    hub->dev->ops->device_remove(child);
}

static void usb_hub_port_service(usb_hub_t *hub, uint8_t port, bool redo) {
    uint16_t status = 0, change = 0;

    if (usb_hub_port_status(hub, port, &status, &change) < 0)
        return;

    for (size_t i = 0; i < PORT_CHANGE_COUNT; i++) {
        if (change & port_changes[i].change)
            usb_hub_port_feature(hub, USB_HUB_REQ_CLEAR, port, port_changes[i].feature);
    }

    serial_printf(LOG_INFO "usbhub: %04x:%04x port %u status 0x%04x change 0x%04x\n",
                  hub->dev->vendor_id, hub->dev->product_id, port, status, change);

    if ((change & USB_PORT_CH_OVER_CURRENT) && (status & USB_PORT_ST_OVER_CURRENT))
        serial_printf(LOG_WARN "usbhub: port %u is over current\n", port);

    bool present = status & USB_PORT_ST_CONNECTED;
    bool replug  = change & USB_PORT_CH_CONNECTION;

    // the hub shuts a port by itself when the bus gets noisy
    if ((change & USB_PORT_CH_ENABLE) && present && hub->child[port] && !(status & USB_PORT_ST_ENABLED))
        redo = true;

    if (replug)
        hub->resets[port] = 0;
    else if (!redo && (present || !hub->child[port]))
        return;

    if (!replug && redo && present && ++hub->resets[port] > USB_HUB_MAX_RESETS) {
        serial_printf(LOG_WARN "usbhub: port %u wont stay up, leaving it until its replugged\n", port);
        return;
    }

    usb_hub_port_remove(hub, port);
    if (!present)
        return;

    usb_delay_ms(USB_HUB_DEBOUNCE_MS);
    if (usb_hub_port_status(hub, port, &status, &change) < 0)
        return;
    if (status & USB_PORT_ST_CONNECTED)
        usb_hub_port_connect(hub, port);
    else
        serial_printf(LOG_INFO "usbhub: port %u gone again before it settled\n", port);
}

// bit 0, the hub has news about itself and keeps saying so until its acked
static void usb_hub_self_service(usb_hub_t *hub) {
    uint16_t words[2] = { 0, 0 };
    usb_setup_t setup = {
        .request_type = USB_REQ_DIR_IN | USB_REQ_TYPE_CLASS | USB_REQ_RCPT_DEVICE,
        .request      = USB_HUB_REQ_GET_STATUS,
        .length       = sizeof(words),
    };

    if (usb_control(hub->dev, &setup, words) < (int)sizeof(words))
        return;
    serial_printf(LOG_WARN "usbhub: %04x:%04x hub status 0x%04x change 0x%04x\n",
                  hub->dev->vendor_id, hub->dev->product_id, words[0], words[1]);

    setup.request_type = USB_REQ_DIR_OUT | USB_REQ_TYPE_CLASS | USB_REQ_RCPT_DEVICE;
    setup.request      = USB_HUB_REQ_CLEAR;
    setup.length       = 0;
    if (words[1] & (1u << USB_HUB_C_LOCAL_POWER)) {
        setup.value = USB_HUB_C_LOCAL_POWER;
        usb_control(hub->dev, &setup, NULL);
    }
    if (words[1] & (1u << USB_HUB_C_OVER_CURRENT)) {
        setup.value = USB_HUB_C_OVER_CURRENT;
        usb_control(hub->dev, &setup, NULL);
    }
}

// usb_service, everything here waits on control transfers
static void usb_hub_service(void) {
    for (int i = 0; i < USB_HUB_MAX; i++) {
        usb_hub_t *hub = &hubs[i];
        if (!hub->used || hub->dev->gone)
            continue;

        uint32_t changed = __atomic_exchange_n(&hub->changed, 0, __ATOMIC_RELAXED);
        uint32_t redo    = __atomic_exchange_n(&hub->redo, 0, __ATOMIC_RELAXED);

        if (changed & 1)
            usb_hub_self_service(hub);

        for (uint8_t port = 1; port <= hub->ports; port++) {
            if (!hub->used || hub->dev->gone)
                break;
            if ((changed | redo) & (1u << port))
                usb_hub_port_service(hub, port, redo & (1u << port));
        }
    }
}

static bool usb_hub_read_desc(usb_hub_t *hub, usb_hub_desc_t *desc) {
    uint8_t     type  = hub->super ? USB_DESC_SS_HUB : USB_DESC_HUB;
    usb_setup_t setup = {
        .request_type = USB_REQ_DIR_IN | USB_REQ_TYPE_CLASS | USB_REQ_RCPT_DEVICE,
        .request      = USB_REQ_GET_DESCRIPTOR,
        .value        = (uint16_t)(type << 8),
        .length       = sizeof(*desc),
    };
    return usb_control(hub->dev, &setup, desc) >= (int)sizeof(*desc);
}

static bool usb_hub_attach(usb_device_t *dev, const usb_endpoint_desc_t *ep) {
    usb_hub_t *hub = NULL;
    for (int i = 0; i < USB_HUB_MAX; i++) {
        if (!hubs[i].used) {
            hub = &hubs[i];
            break;
        }
    }
    if (!hub) {
        serial_printf(LOG_WARN "usbhub: too many hubs\n");
        return false;
    }

    memset(hub, 0, sizeof(*hub));
    hub->dev   = dev;
    hub->super = dev->speed == USB_SPEED_SUPER;

    // usb3 hubs have to know where they sit to read the route string
    if (hub->super) {
        usb_setup_t depth = {
            .request_type = USB_REQ_DIR_OUT | USB_REQ_TYPE_CLASS | USB_REQ_RCPT_DEVICE,
            .request      = USB_HUB_REQ_SET_DEPTH,
            .value        = dev->depth,
        };
        if (usb_control(dev, &depth, NULL) < 0)
            serial_printf(LOG_WARN "usbhub: SET_HUB_DEPTH failed\n");
    }

    usb_hub_desc_t desc;
    memset(&desc, 0, sizeof(desc));
    if (!usb_hub_read_desc(hub, &desc)) {
        serial_printf(LOG_WARN "usbhub: cant read the hub descriptor\n");
        return false;
    }

    hub->ports = desc.ports > USB_HUB_MAX_PORTS ? USB_HUB_MAX_PORTS : desc.ports;
    serial_printf(LOG_INFO "usbhub: %s speed hub, %u ports, characteristics 0x%04x, power good %ums\n",
                  usb_speed_name(dev->speed), desc.ports, desc.characteristics, desc.power_good * 2);

    if (usb_interrupt_in(dev, ep, usb_hub_status_report, hub) < 0)
        return false;
    if (dev->ops->hub_setup(dev, desc.ports, USB_HUB_THINK_TIME(desc.characteristics)) < 0) {
        serial_printf(LOG_WARN "usbhub: controller wont take it as a hub\n");
        return false;
    }

    for (uint8_t port = 1; port <= hub->ports; port++)
        usb_hub_port_feature(hub, USB_HUB_REQ_SET, port, USB_PORT_POWER);
    usb_delay_ms((uint64_t)desc.power_good * 2);

    // usb_service looks at every port once to pick up whats already in there
    hub->changed = ((1ull << (hub->ports + 1)) - 1) & ~1u;
    hub->used    = true;

    serial_printf(LOG_OK "usbhub: hub %04x:%04x at depth %u on port %u\n",
                  dev->vendor_id, dev->product_id, dev->depth, dev->port);
    return true;
}

static bool usb_hub_probe(usb_device_t *dev, const uint8_t *config, size_t len) {
    bool in_hub = false;

    for (size_t off = 0; off + 2 <= len; ) {
        uint8_t desc_len  = config[off];
        uint8_t desc_type = config[off + 1];
        if (desc_len < 2 || off + desc_len > len) break;

        if (desc_type == USB_DESC_INTERFACE && desc_len >= sizeof(usb_interface_desc_t)) {
            const usb_interface_desc_t *iface = (const usb_interface_desc_t *)&config[off];
            in_hub = iface->class_code == USB_CLASS_HUB && iface->alt_setting == 0;
        } else if (desc_type == USB_DESC_ENDPOINT && in_hub && desc_len >= sizeof(usb_endpoint_desc_t)) {
            const usb_endpoint_desc_t *ep = (const usb_endpoint_desc_t *)&config[off];
            if (!(ep->address & USB_ENDPOINT_IN) ||
                (ep->attributes & USB_ENDPOINT_TYPE_MASK) != USB_ENDPOINT_INTERRUPT) {
                off += desc_len;
                continue;
            }

            if (!dev->ops->device_add || !dev->ops->device_remove || !dev->ops->hub_setup) {
                serial_printf(LOG_WARN "usbhub: %s cant drive hubs yet\n", dev->ops->name);
                return false;
            }
            if (dev->depth >= USB_HUB_MAX_DEPTH) {
                serial_printf(LOG_WARN "usbhub: hub at depth %u is too deep\n", dev->depth);
                return false;
            }
            return usb_hub_attach(dev, ep);
        }

        off += desc_len;
    }
    return false;
}

static void usb_hub_disconnect(usb_device_t *dev) {
    usb_hub_t *hub = usb_hub_of(dev);
    if (!hub)
        return;

    hub->used = false;
    for (uint8_t port = 1; port <= hub->ports; port++)
        usb_hub_port_remove(hub, port);

    serial_printf(LOG_INFO "usbhub: hub %04x:%04x gone\n", dev->vendor_id, dev->product_id);
}

void usb_hub_port_redo(usb_device_t *dev) {
    usb_hub_t *hub = usb_hub_of(dev->parent);
    if (hub && dev->port <= hub->ports)
        __atomic_fetch_or(&hub->redo, 1u << dev->port, __ATOMIC_RELAXED);
}

void usb_hub_port_ok(usb_device_t *dev) {
    usb_hub_t *hub = usb_hub_of(dev->parent);
    if (hub && dev->port <= hub->ports)
        hub->resets[dev->port] = 0;
}

const usb_class_t usb_hub_class = {
    .name       = "usb hub",
    .probe      = usb_hub_probe,
    .disconnect = usb_hub_disconnect,
    .hotplug    = usb_hub_service,
};
