/*	$OpenBSD: print-gre.c,v 1.12 2016/12/13 06:40:21 dlg Exp $	*/

/*
 * Copyright (c) 2002 Jason L. Wright (jason@thought.net)
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * tcpdump filter for GRE - Generic Routing Encapsulation
 * RFC1701 (GRE), RFC1702 (GRE IPv4), and RFC2637 (Enhanced GRE)
 */

#include <sys/time.h>
#include <sys/uio.h>
#include <sys/socket.h>

#include <netinet/in.h>
#include <netinet/ip.h>
#include <arpa/inet.h>

#include <net/ethertypes.h>

#include <stdio.h>
#include <string.h>

#include "interface.h"
#include "addrtoname.h"
#include "extract.h"

#define	GRE_CP		0x8000		/* checksum present */
#define	GRE_RP		0x4000		/* routing present */
#define	GRE_KP		0x2000		/* key present */
#define	GRE_SP		0x1000		/* sequence# present */
#define	GRE_sP		0x0800		/* source routing */
#define	GRE_RECRS	0x0700		/* recursion count */
#define	GRE_AP		0x0080		/* acknowledgment# present */
#define	GRE_VERS	0x0007		/* protocol version */

/* source route entry types */
#define	GRESRE_IP	0x0800		/* IP */
#define	GRESRE_ASN	0xfffe		/* ASN */

#define NVGRE_VSID_MASK		0xffffff00U
#define NVGRE_VSID_SHIFT	8
#define NVGRE_FLOWID_MASK	0x000000ffU
#define NVGRE_FLOWID_SHIFT	0

void gre_print_0(const u_char *, u_int);
void gre_print_1(const u_char *, u_int);
void gre_sre_print(u_int16_t, u_int8_t, u_int8_t, const u_char *, u_int);
void gre_sre_ip_print(u_int8_t, u_int8_t, const u_char *, u_int);
void gre_sre_asn_print(u_int8_t, u_int8_t, const u_char *, u_int);

void
gre_print(const u_char *bp, u_int length)
{
	u_int len = length, vers;

	if (bp + len > snapend)
		len = snapend - bp;

	if (len < 2) {
		printf("[|gre]");
		return;
	}
	vers = EXTRACT_16BITS(bp) & GRE_VERS;

	switch (vers) {
	case 0:
		gre_print_0(bp, len);
		break;
	case 1:
		gre_print_1(bp, len);
		break;
	default:
		printf("gre-unknown-version=%u", vers);
		break;
	}
}

void
gre_print_0(const u_char *bp, u_int length)
{
	u_int len = length;
	u_int16_t flags, prot;

	flags = EXTRACT_16BITS(bp);
	if (vflag) {
		printf("[%s%s%s%s%s] ",
		    (flags & GRE_CP) ? "C" : "",
		    (flags & GRE_RP) ? "R" : "",
		    (flags & GRE_KP) ? "K" : "",
		    (flags & GRE_SP) ? "S" : "",
		    (flags & GRE_sP) ? "s" : "");
	}

	len -= 2;
	bp += 2;

	if (len < 2)
		goto trunc;
	prot = EXTRACT_16BITS(bp);
	printf("%s", etherproto_string(prot));

	len -= 2;
	bp += 2;

	if ((flags & GRE_CP) | (flags & GRE_RP)) {
		if (len < 2)
			goto trunc;
		if (vflag)
			printf(" sum 0x%x", EXTRACT_16BITS(bp));
		bp += 2;
		len -= 2;

		if (len < 2)
			goto trunc;
		printf(" off 0x%x", EXTRACT_16BITS(bp));
		bp += 2;
		len -= 2;
	}

	if (flags & GRE_KP) {
		uint32_t key, vsid;

		if (len < 4)
			goto trunc;
		key = EXTRACT_32BITS(bp);

		/* maybe NVGRE? */
		if (flags == (GRE_KP | 0) && prot == ETHERTYPE_TRANSETHER) {
			vsid = (key & NVGRE_VSID_MASK) >> NVGRE_VSID_SHIFT;
			printf(" NVGRE vsid=%u (0x%x)+flowid=0x%02x /",
			    vsid, vsid,
			    (key & NVGRE_FLOWID_MASK) >> NVGRE_FLOWID_SHIFT);
		}
		printf(" key=%u (0x%x)", key, key);
		bp += 4;
		len -= 4;
	}

	if (flags & GRE_SP) {
		if (len < 4)
			goto trunc;
		printf(" seq %u", EXTRACT_32BITS(bp));
		bp += 4;
		len -= 4;
	}

	if (flags & GRE_RP) {
		for (;;) {
			u_int16_t af;
			u_int8_t sreoff;
			u_int8_t srelen;

			if (len < 4)
				goto trunc;
			af = EXTRACT_16BITS(bp);
			sreoff = *(bp + 2);
			srelen = *(bp + 3);
			bp += 4;
			len -= 4;

			if (af == 0 && srelen == 0)
				break;

			gre_sre_print(af, sreoff, srelen, bp, len);

			if (len < srelen)
				goto trunc;
			bp += srelen;
			len -= srelen;
		}
	}

	printf(": ");

	switch (prot) {
	case ETHERTYPE_IP:
		ip_print(bp, len);
		break;
	case ETHERTYPE_IPV6:
		ip6_print(bp, len);
		break;
	case ETHERTYPE_MPLS:
		mpls_print(bp, len);
		break;
	case ETHERTYPE_TRANSETHER:
		ether_print(bp, len);
		break;
	default:
		printf("gre-proto-0x%x", prot);
	}
	return;

trunc:
	printf("[|gre]");
}

void
gre_print_1(const u_char *bp, u_int length)
{
	u_int len = length;
	u_int16_t flags, prot;

	flags = EXTRACT_16BITS(bp);
	len -= 2;
	bp += 2;

	if (vflag) {
		printf("[%s%s%s%s%s%s]",
		    (flags & GRE_CP) ? "C" : "",
		    (flags & GRE_RP) ? "R" : "",
		    (flags & GRE_KP) ? "K" : "",
		    (flags & GRE_SP) ? "S" : "",
		    (flags & GRE_sP) ? "s" : "",
		    (flags & GRE_AP) ? "A" : "");
	}

	if (len < 2)
		goto trunc;
	prot = EXTRACT_16BITS(bp);
	len -= 2;
	bp += 2;

	if (flags & GRE_CP) {
		printf(" cpset!");
		return;
	}
	if (flags & GRE_RP) {
		printf(" rpset!");
		return;
	}
	if ((flags & GRE_KP) == 0) {
		printf(" kpunset!");
		return;
	}
	if (flags & GRE_sP) {
		printf(" spset!");
		return;
	}

	if (flags & GRE_KP) {
		u_int32_t k;

		if (len < 4)
			goto trunc;
		k = EXTRACT_32BITS(bp);
		printf(" call %d", k & 0xffff);
		len -= 4;
		bp += 4;
	}

	if (flags & GRE_SP) {
		if (len < 4)
			goto trunc;
		printf(" seq %u", EXTRACT_32BITS(bp));
		bp += 4;
		len -= 4;
	}

	if (flags & GRE_AP) {
		if (len < 4)
			goto trunc;
		printf(" ack %u", EXTRACT_32BITS(bp));
		bp += 4;
		len -= 4;
	}

	if ((flags & GRE_SP) == 0) {
		printf(" no-payload");
		return;
	}

	printf(": ");

	switch (prot) {
	case ETHERTYPE_PPP:
		printf("gre-ppp-payload");
		break;
	default:
		printf("gre-proto-0x%x", prot);
		break;
	}
	return;

trunc:
	printf("[|gre]");
}

void
gre_sre_print(u_int16_t af, u_int8_t sreoff, u_int8_t srelen,
    const u_char *bp, u_int len)
{
	switch (af) {
	case GRESRE_IP:
		printf(" (rtaf=ip");
		gre_sre_ip_print(sreoff, srelen, bp, len);
		printf(")");
		break;
	case GRESRE_ASN:
		printf(" (rtaf=asn");
		gre_sre_asn_print(sreoff, srelen, bp, len);
		printf(")");
		break;
	default:
		printf(" (rtaf=0x%x)", af);
	}
}
void
gre_sre_ip_print(u_int8_t sreoff, u_int8_t srelen, const u_char *bp, u_int len)
{
	struct in_addr a;
	const u_char *up = bp;

	if (sreoff & 3) {
		printf(" badoffset=%u", sreoff);
		return;
	}
	if (srelen & 3) {
		printf(" badlength=%u", srelen);
		return;
	}
	if (sreoff >= srelen) {
		printf(" badoff/len=%u/%u", sreoff, srelen);
		return;
	}

	for (;;) {
		if (len < 4 || srelen == 0)
			return;

		memcpy(&a, bp, sizeof(a));
		printf(" %s%s",
		    ((bp - up) == sreoff) ? "*" : "",
		    inet_ntoa(a));

		bp += 4;
		len -= 4;
		srelen -= 4;
	}
}

void
gre_sre_asn_print(u_int8_t sreoff, u_int8_t srelen, const u_char *bp, u_int len)
{
	const u_char *up = bp;

	if (sreoff & 1) {
		printf(" badoffset=%u", sreoff);
		return;
	}
	if (srelen & 1) {
		printf(" badlength=%u", srelen);
		return;
	}
	if (sreoff >= srelen) {
		printf(" badoff/len=%u/%u", sreoff, srelen);
		return;
	}

	for (;;) {
		if (len < 2 || srelen == 0)
			return;

		printf(" %s%x",
		    ((bp - up) == sreoff) ? "*" : "",
		    EXTRACT_16BITS(bp));

		bp += 2;
		len -= 2;
		srelen -= 2;
	}
}
