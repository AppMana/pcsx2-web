// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include <pcap.h>
#include <string.h>

static void pcap_stub_error(char* errbuf)
{
	if (errbuf)
		strncpy(errbuf, "libpcap is not available in the browser", PCAP_ERRBUF_SIZE);
}

pcap_t* pcap_open_live(const char* device, int snaplen, int promisc, int to_ms, char* errbuf)
{
	pcap_stub_error(errbuf);
	return NULL;
}

int pcap_findalldevs(pcap_if_t** alldevsp, char* errbuf)
{
	*alldevsp = NULL;
	pcap_stub_error(errbuf);
	return PCAP_ERROR;
}

void pcap_freealldevs(pcap_if_t* alldevs)
{
}

void pcap_close(pcap_t* p)
{
}

int pcap_compile(pcap_t* p, struct bpf_program* fp, const char* str, int optimize, bpf_u_int32 netmask)
{
	return PCAP_ERROR;
}

int pcap_setfilter(pcap_t* p, struct bpf_program* fp)
{
	return PCAP_ERROR;
}

void pcap_freecode(struct bpf_program* fp)
{
}

int pcap_datalink(pcap_t* p)
{
	return PCAP_ERROR_NOT_ACTIVATED;
}

const char* pcap_datalink_val_to_name(int dlt)
{
	return NULL;
}

char* pcap_geterr(pcap_t* p)
{
	return "libpcap is not available in the browser";
}

int pcap_next_ex(pcap_t* p, struct pcap_pkthdr** pkt_header, const u_char** pkt_data)
{
	return PCAP_ERROR;
}

int pcap_sendpacket(pcap_t* p, const u_char* buf, int size)
{
	return PCAP_ERROR;
}

int pcap_setnonblock(pcap_t* p, int nonblock, char* errbuf)
{
	pcap_stub_error(errbuf);
	return PCAP_ERROR;
}
