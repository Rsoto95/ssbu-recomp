/* POSIX UPnP stand-in: the Windows build maps a UDP port with the COM IUPnPNAT API; Linux has no equivalent here.
 * The host still works on a LAN or over the internet with the UDP port forwarded by hand. */
#include <stdint.h>
#include <stdio.h>
#include <stddef.h>

static int upnp_map_udp(uint16_t port, uint32_t lan_ip_host_order, const char *desc, char *ext, size_t ext_n, char *why, size_t why_n) {
    (void)lan_ip_host_order; (void)desc;
    if (ext_n) ext[0] = 0;
    snprintf(why, why_n, "UPnP is not available on Linux: forward UDP port %u on your router", port);
    return 0;
}
static void upnp_unmap_udp(uint16_t port) { (void)port; }
static int upnp_probe(char *why, size_t why_n) {
    snprintf(why, why_n, "UPnP is not available on Linux: forward the UDP port manually");
    return 0;
}
