#include <usb/usb_hcd.h>
#include <cpu/mmio.h>
#include <mm/dma.h>
#include <ACPI/acpi_hpet.h>

bool usb_timed_out(uint64_t start, uint64_t spins, uint64_t timeout_ms) {
    return hpet_timed_out(start, spins, timeout_ms);
}

void usb_delay_ms(uint64_t ms) {
    hpet_busy_wait_ms(ms);
}

bool usb_wait32(uint64_t base, uint32_t reg, uint32_t mask, bool want_set, uint64_t timeout_ms) {
    return mmio_wait32(base, reg, mask, want_set, timeout_ms);
}

void usb_pci_enable(const pci_device_t *pci_dev) {
    pci_enable(pci_dev);
}

uint64_t usb_bar0_size(const pci_device_t *pci_dev) {
    return pci_bar_size(pci_dev, 0);
}

uint64_t usb_map_bar0(const pci_device_t *pci_dev, uint64_t size) {
    return pci_map_bar(pci_dev, 0, size);
}

void *usb_dma_alloc(size_t pages, paddr_t *phys, bool low_only) {
    return dma_alloc(pages, phys, low_only);
}
