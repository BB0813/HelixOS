/*
 * NIC backend selection: prefer e1000 (simpler, reliable on QEMU),
 * fall back to virtio-net-pci.
 */
#include "helix/net.h"
#include "helix/kprintf.h"

static int g_backend; /* 0=none 1=e1000 2=virtio */

int nic_init(void)
{
    g_backend = 0;
    if (e1000_init() == 0) {
        g_backend = 1;
        kprintf("[net] backend=e1000\n");
        return 0;
    }
    if (virtio_net_init() == 0) {
        g_backend = 2;
        kprintf("[net] backend=virtio-net\n");
        return 0;
    }
    kprintf("[net] no NIC backend\n");
    return -1;
}

int nic_ready(void)
{
    if (g_backend == 1)
        return e1000_ready();
    if (g_backend == 2)
        return virtio_net_ready();
    return 0;
}

void nic_get_mac(u8 mac[6])
{
    if (g_backend == 1)
        e1000_get_mac(mac);
    else if (g_backend == 2)
        virtio_net_get_mac(mac);
    else {
        for (int i = 0; i < 6; i++)
            mac[i] = 0;
    }
}

int nic_send(const void *frame, u32 len)
{
    if (g_backend == 1)
        return e1000_send(frame, len);
    if (g_backend == 2)
        return virtio_net_send(frame, len);
    return -1;
}

int nic_recv(void *buf, u32 buflen)
{
    if (g_backend == 1)
        return e1000_recv(buf, buflen);
    if (g_backend == 2)
        return virtio_net_recv(buf, buflen);
    return 0;
}
