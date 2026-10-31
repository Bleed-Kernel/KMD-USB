#pragma once

#include <usb/usb.h>

#define USB_HUB_MAX             8
#define USB_HUB_MAX_PORTS       31      // status bitmap is a uint32, bit 0 is the hub itself
#define USB_HUB_MAX_DEPTH       5
#define USB_HUB_ATTACH_TRIES    3
#define USB_HUB_MAX_RESETS      3
#define USB_HUB_RETRY_MS        50
#define USB_HUB_DEBOUNCE_MS     100
#define USB_HUB_RESET_TIMEOUT_MS 500
#define USB_HUB_RESET_POLL_MS   10
#define USB_HUB_RECOVERY_MS     10

// class requests, the port ones go to the "other" recipient
#define USB_HUB_REQ_GET_STATUS  0
#define USB_HUB_REQ_CLEAR       1
#define USB_HUB_REQ_SET         3
#define USB_HUB_REQ_SET_DEPTH   12

#define USB_HUB_C_LOCAL_POWER   0
#define USB_HUB_C_OVER_CURRENT  1

// port feature selectors
#define USB_PORT_RESET          4
#define USB_PORT_POWER          8
#define USB_PORT_C_CONNECTION   16
#define USB_PORT_C_ENABLE       17
#define USB_PORT_C_SUSPEND      18
#define USB_PORT_C_OVER_CURRENT 19
#define USB_PORT_C_RESET        20
#define USB_PORT_C_LINK_STATE   25
#define USB_PORT_C_CONFIG_ERROR 26
#define USB_PORT_C_BH_RESET     29

// wPortStatus
#define USB_PORT_ST_CONNECTED   (1u << 0)
#define USB_PORT_ST_ENABLED     (1u << 1)
#define USB_PORT_ST_OVER_CURRENT (1u << 3)
#define USB_PORT_ST_LOW_SPEED   (1u << 9)   // usb2 hubs only
#define USB_PORT_ST_HIGH_SPEED  (1u << 10)

// wPortChange
#define USB_PORT_CH_CONNECTION  (1u << 0)
#define USB_PORT_CH_ENABLE      (1u << 1)
#define USB_PORT_CH_OVER_CURRENT (1u << 3)
#define USB_PORT_CH_RESET       (1u << 4)
#define USB_PORT_CH_BH_RESET    (1u << 5)

typedef struct __attribute__((packed)) usb_hub_desc {
    uint8_t  length;
    uint8_t  type;
    uint8_t  ports;
    uint16_t characteristics;
    uint8_t  power_good;        // 2ms units
    uint8_t  current;
} usb_hub_desc_t;

#define USB_HUB_THINK_TIME(c)   (((c) >> 5) & 3)

extern const usb_class_t usb_hub_class;

// Reenumerate whatever sits behind this hub port, the device stopped making sense
void usb_hub_port_redo(usb_device_t *dev);

// A device behind a hub sent a good report, its reset budget starts over
void usb_hub_port_ok(usb_device_t *dev);
