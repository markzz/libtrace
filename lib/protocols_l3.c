/* Protocol decodes for Layer 3 protocols */
#include "libtrace.h"
#include "protocols.h"
#include <assert.h>

libtrace_ip_t *trace_get_ip(libtrace_packet_t *packet) 
{
	uint16_t ethertype;
	void *ret;

	uint32_t remaining = trace_get_capture_length(packet);

	ret = trace_get_layer3(packet,&ethertype,&remaining);

	if (!ret || ethertype!=0x0800)
		return NULL;

	/* Not an IPv4 packet */
	if (((libtrace_ip_t*)ret)->ip_v != 4)
		return NULL;

	return (libtrace_ip_t*)ret;
}

libtrace_ip6_t *trace_get_ip6(libtrace_packet_t *packet) 
{
	uint16_t ethertype;
	void *ret;

	uint32_t remaining = trace_get_capture_length(packet);

	ret = trace_get_layer3(packet,&ethertype,&remaining);

	if (!ret || ethertype!=0x86DD)
		return NULL;

	return (libtrace_ip6_t*)ret;
}

#define SW_IP_OFFMASK 0xff1f

DLLEXPORT void *trace_get_payload_from_ip(libtrace_ip_t *ipptr, uint8_t *prot,
		uint32_t *remaining) 
{
        void *trans_ptr = 0;

        if ((ntohs(ipptr->ip_off) & SW_IP_OFFMASK) != 0) {
		return NULL;
	}

	if (remaining) {
		if (*remaining<(ipptr->ip_hl*4U)) {
			return NULL;
		}
		*remaining-=(ipptr->ip_hl * 4);
	}

        trans_ptr = (void *)((char *)ipptr + (ipptr->ip_hl * 4));

	if (prot) *prot = ipptr->ip_p;

        return trans_ptr;
}

void *trace_get_payload_from_ip6(libtrace_ip6_t *ipptr, uint8_t *prot,
		uint32_t *remaining) 
{
	void *payload = (char*)ipptr+sizeof(libtrace_ip6_t);
	uint8_t nxt = ipptr->nxt;

	if (remaining) {
		if (*remaining<sizeof(libtrace_ip6_t))
			return NULL;
		*remaining-=sizeof(libtrace_ip6_t);
	}

	while(1) {
		switch (nxt) {
			case 0: /* hop by hop options */
			case 43: /* routing */
			case 44: /* fragment */
			case 50: /* ESP */
			case 51: /* AH */
			case 60: /* Destination options */
				{
					uint16_t len=((libtrace_ip6_ext_t*)payload)->len
					+sizeof(libtrace_ip6_ext_t);

					if (remaining) {
						if (*remaining < len) {
							/* Snap too short */
							return NULL;
						}
						*remaining-=len;
					}

					payload=(char*)payload+len;
					nxt=((libtrace_ip6_ext_t*)payload)->nxt;
					continue;
				}
			default:
				if (prot) *prot=nxt;
				return payload;
		}
	}
}

DLLEXPORT void *trace_get_layer3(const libtrace_packet_t *packet,
		uint16_t *ethertype,
		uint32_t *remaining)
{
	void *iphdr;
	uint16_t dummy_ethertype;
	void *link;
	uint32_t dummy_remaining;
	libtrace_linktype_t linktype;

	if (!ethertype) ethertype=&dummy_ethertype;

	if (!remaining) remaining=&dummy_remaining;

	/* use l3 cache */
	if (packet->l3_header)
	{
		link = trace_get_packet_buffer(packet,&linktype,remaining);

		if (!link)
			return NULL;

		*ethertype = packet->l3_ethertype;
		*remaining -= (packet->l3_header - link);

		return packet->l3_header;
	}

	link = trace_get_layer2(packet,&linktype,remaining);

	iphdr = trace_get_payload_from_layer2(
			link,
			linktype,
			ethertype,
			remaining);

	if (!iphdr)
		return NULL;

	for(;;) {
		switch(*ethertype) {
		case 0x8100: /* VLAN */
			iphdr=trace_get_vlan_payload_from_ethernet_payload(
					  iphdr,ethertype,NULL);
			continue;
		case 0x8847: /* MPLS */
			iphdr=trace_get_mpls_payload_from_ethernet_payload(
					  iphdr,ethertype,NULL);

			if (iphdr && ethertype == 0x0) {
				iphdr=trace_get_payload_from_ethernet(
						iphdr,ethertype,NULL);
			}
			continue;
		default:
			break;
		}

		break;
	}

	/* Store values in the cache for later */
	/* Cast away constness, nasty, but this is just a cache */
	((libtrace_packet_t*)packet)->l3_ethertype = *ethertype;
	((libtrace_packet_t*)packet)->l3_header = iphdr;

	return iphdr;
}

/* parse an ip or tcp option
 * @param[in,out] ptr	the pointer to the current option
 * @param[in,out] len	the length of the remaining buffer
 * @param[out] type	the type of the option
 * @param[out] optlen 	the length of the option
 * @param[out] data	the data of the option
 *
 * @returns bool true if there is another option (and the fields are filled in)
 *               or false if this was the last option.
 *
 * This updates ptr to point to the next option after this one, and updates
 * len to be the number of bytes remaining in the options area.  Type is updated
 * to be the code of this option, and data points to the data of this option,
 * with optlen saying how many bytes there are.
 *
 * @note Beware of fragmented packets.
 * @author Perry Lorier
 */
DLLEXPORT int trace_get_next_option(unsigned char **ptr,int *len,
			unsigned char *type,
			unsigned char *optlen,
			unsigned char **data)
{
	if (*len<=0)
		return 0;
	*type=**ptr;
	switch(*type) {
		case 0: /* End of options */
			return 0;
		case 1: /* Pad */
			(*ptr)++;
			(*len)--;
			return 1;
		default:
			*optlen = *(*ptr+1);
			if (*optlen<2)
				return 0; /* I have no idea wtf is going on
					   * with these packets
					   */
			(*len)-=*optlen;
			(*data)=(*ptr+2);
			(*ptr)+=*optlen;
			if (*len<0)
				return 0;
			return 1;
	}
	assert(0);
}

DLLEXPORT struct sockaddr *trace_get_source_address(
		const libtrace_packet_t *packet, struct sockaddr *addr)
{
	uint16_t ethertype;
	uint32_t remaining;
	void *l3;
	struct ports_t *ports;
	static struct sockaddr_storage dummy;

	if (!addr)
		addr=(struct sockaddr*)&dummy;

	l3 = trace_get_layer3(packet,&ethertype,&remaining);

	if (!l3)
		return NULL;

	switch (ethertype) {
		case 0x0800: /* IPv4 */
		{
			struct sockaddr_in *addr4=(struct sockaddr_in*)addr;
			libtrace_ip_t *ip = (libtrace_ip_t*)l3;
			ports = (struct ports_t*)
				trace_get_payload_from_ip(ip,NULL,&remaining);
			addr4->sin_family=AF_INET;
			if (ports && remaining>=sizeof(*ports))
				addr4->sin_port=ports->src;
			else
				addr4->sin_port=0;
			addr4->sin_addr=ip->ip_src;
			return addr;
		}
		case 0x86DD: /* IPv6 */
		{
			struct sockaddr_in6 *addr6=(struct sockaddr_in6*)addr;
			libtrace_ip6_t *ip6 = (libtrace_ip6_t*)l3;
			ports = (struct ports_t*)
				trace_get_payload_from_ip6(ip6,NULL,&remaining);
			addr6->sin6_family=AF_INET6;
			if (ports && remaining>=sizeof(*ports))
				addr6->sin6_port=ports->src;
			else
				addr6->sin6_port=0;
			addr6->sin6_flowinfo=0;
			addr6->sin6_addr=ip6->ip_src;
			return addr;
		}
		default:
			return NULL;
	}
}

DLLEXPORT struct sockaddr *trace_get_destination_address(
		const libtrace_packet_t *packet, struct sockaddr *addr)
{
	uint16_t ethertype;
	uint32_t remaining;
	void *l3;
	struct ports_t *ports;
	static struct sockaddr_storage dummy;

	if (!addr)
		addr=(struct sockaddr*)&dummy;

	l3 = trace_get_layer3(packet,&ethertype,&remaining);

	if (!l3)
		return NULL;

	switch (ethertype) {
		case 0x0800: /* IPv4 */
		{
			struct sockaddr_in *addr4=(struct sockaddr_in*)addr;
			libtrace_ip_t *ip = (libtrace_ip_t*)l3;
			ports = (struct ports_t*)
				trace_get_payload_from_ip(ip,NULL,&remaining);
			addr4->sin_family=AF_INET;
			if (ports && remaining>=sizeof(*ports))
				addr4->sin_port=ports->dst;
			else
				addr4->sin_port=0;
			addr4->sin_addr=ip->ip_dst;
			return addr;
		}
		case 0x86DD: /* IPv6 */
		{
			struct sockaddr_in6 *addr6=(struct sockaddr_in6*)addr;
			libtrace_ip6_t *ip6 = (libtrace_ip6_t*)l3;
			ports = (struct ports_t*)
				trace_get_payload_from_ip6(ip6,NULL,&remaining);
			addr6->sin6_family=AF_INET6;
			if (ports && remaining>=sizeof(*ports))
				addr6->sin6_port=ports->dst;
			else
				addr6->sin6_port=0;
			addr6->sin6_flowinfo=0;
			addr6->sin6_addr=ip6->ip_dst;
			return addr;
		}
		default:
			return NULL;
	}
}

