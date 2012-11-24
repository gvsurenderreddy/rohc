/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

/**
 * @file   test_non_regression.c
 * @brief  ROHC non-regression test program
 * @author Didier Barvaux <didier.barvaux@toulouse.viveris.com>
 * @author Didier Barvaux <didier@barvaux.org>
 * @author David Moreau from TAS
 *
 * Introduction
 * ------------
 *
 * The program takes a flow of IP packets as input (in the PCAP format) and
 * tests the ROHC compression/decompression library with them. The program
 * also tests the feedback mechanism.
 *
 * Details
 * -------
 *
 * The program defines two compressor/decompressor pairs and sends the flow
 * of IP packet through Compressor 1 and Decompressor 1 (flow A) and through
 * Compressor 2 and Decompressor 2 (flow B). See the figure below.
 *
 * The feedback for flow A is sent by Decompressor 1 to Compressor 1 via
 * Compressor 2 and Decompressor 2. The feedback for flow  B is sent by
 * Decompressor 2 to Compressor 2 via Compressor 1 and Decompressor 1.
 *
 *          +-- IP packets                             IP packets <--+
 *          |   flow A (input)                    flow A (output)    |
 *          |                                                        |
 *          |    +----------------+    ROHC    +----------------+    |
 *          +--> |                |            |                | ---+
 *               |  Compressor 1  | ---------> | Decompressor 1 |
 *          +--> |                |            |                | ---+
 *          |    +----------------+            +----------------+    |
 * feedback |                                                        | feedback
 * flow B   |                                                        | flow A
 *          |    +----------------+     ROHC   +----------------+    |
 *          +--- |                |            |                | <--+
 *               | Decompressor 2 | <--------- |  Compressor 2  |
 *          +--- |                |            |                | <--+
 *          |    +----------------+            +----------------+    |
 *          |                                                        |
 *          +--> IP packets                             IP packets --+
 *               flow B (output)                    flow B (input)
 *
 * Checks
 * ------
 *
 * The program checks for the status of the compression and decompression
 * processes. The program also compares input IP packets from flow A (resp.
 * flow B) with output IP packets from flow A (resp. flow B).
 *
 * The program optionally compares the ROHC packets generated with the ones
 * given as input to the program.
 *
 * Output
 * ------
 *
 * The program outputs XML containing the compression/decompression/comparison
 * status of every packets of flow A and flow B on stdout. It also outputs the
 * log of the different processes (startup, compression, decompression,
 * comparison and shutdown).
 *
 * The program optionally outputs the ROHC packets in a PCAP packet.
 */

#include "test.h"
#include "config.h" /* for HAVE_*_H */

/* system includes */
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <string.h>
#if HAVE_WINSOCK2_H == 1
#  include <winsock2.h> /* for ntohs() on Windows */
#endif
#if HAVE_ARPA_INET_H == 1
#  include <arpa/inet.h> /* for ntohs() on Linux */
#endif
#include <errno.h>
#include <assert.h>
#include <stdarg.h>

/* includes for network headers */
#include <protocols/ipv4.h>
#include <protocols/ipv6.h>

/* include for the PCAP library */
#if HAVE_PCAP_PCAP_H == 1
#  include <pcap/pcap.h>
#elif HAVE_PCAP_H == 1
#  include <pcap.h>
#else
#  error "pcap.h header not found, did you specified --enable-rohc-tests \
for ./configure ? If yes, check configure output and config.log"
#endif

/* ROHC includes */
#include <rohc.h>
#include <rohc_comp.h>
#include <rohc_decomp.h>
#include "config.h" /* for RTP_BIT_TYPE definition */


/// The program version
#define TEST_VERSION  "ROHC non-regression test application, version 0.1\n"


struct test_stats
{
	unsigned long comp_pre;
	unsigned long comp_post;
	unsigned long comp_nr_pkts_per_profile[ROHC_PROFILE_UDPLITE + 1];
	unsigned long comp_nr_pkts_per_mode[R_MODE + 1];
	unsigned long comp_nr_pkts_per_state[SO + 1];
	unsigned long comp_nr_pkts_per_pkt_type[PACKET_UNKNOWN];
	unsigned long comp_nr_reused_cid;
};


/* prototypes of private functions */
static void usage(void);
static int test_comp_and_decomp(const int use_large_cid,
                                const unsigned int max_contexts,
                                char *src_filename,
                                char *ofilename,
                                char *cmp_filename,
                                const char *rohc_size_ofilename);
static int compress_decompress(struct rohc_comp *comp,
                               struct rohc_decomp *decomp,
                               int num_comp,
                               int num_packet,
                               struct pcap_pkthdr header,
                               unsigned char *packet,
                               int link_len_src,
                               int use_large_cid,
                               pcap_dumper_t *dumper,
                               unsigned char *cmp_packet,
                               int cmp_size,
                               int link_len_cmp,
                               FILE *size_output_file,
                               struct test_stats *stats);

static void print_rohc_traces(const rohc_trace_level_t level,
                              const rohc_trace_entity_t entity,
                              const int profile,
                              const char *const format,
                              ...)
	__attribute__((format(printf, 4, 5), nonnull(4)));

static int gen_false_random_num(const struct rohc_comp *const comp,
                                void *const user_context)
	__attribute__((nonnull(1)));

static bool rtp_detect_cb(const unsigned char *const ip,
                          const unsigned char *const udp,
                          const unsigned char *const payload,
                          const unsigned int payload_size,
                          void *const rtp_private)
	__attribute__((nonnull(1, 2, 3), warn_unused_result));

#if 0
static void show_rohc_stats(struct rohc_comp *comp1, struct rohc_decomp *decomp1,
                            struct rohc_comp *comp2, struct rohc_decomp *decomp2);
#endif

static int compare_packets(unsigned char *pkt1, int pkt1_size,
                           unsigned char *pkt2, int pkt2_size);


/** Whether the application runs in verbose mode or not */
static int is_verbose;


/**
 * @brief Main function for the ROHC test program
 *
 * @param argc The number of program arguments
 * @param argv The program arguments
 * @return     The unix return code:
 *              \li 0 in case of success,
 *              \li 1 in case of failure,
 *              \li 77 in case test is skipped
 */
int main(int argc, char *argv[])
{
	char *cid_type = NULL;
	char *rohc_size_ofilename = NULL;
	char *src_filename = NULL;
	char *ofilename = NULL;
	char *cmp_filename = NULL;
	int max_contexts = ROHC_SMALL_CID_MAX + 1;
	int status = 1;
	int use_large_cid;
	int args_used;

	/* set to quiet mode by default */
	is_verbose = 0;

	/* parse program arguments, print the help message in case of failure */
	if(argc <= 1)
	{
		usage();
		goto error;
	}

	for(argc--, argv++; argc > 0; argc -= args_used, argv += args_used)
	{
		args_used = 1;

		if(!strcmp(*argv, "-v"))
		{
			/* print version */
			printf(TEST_VERSION);
			goto error;
		}
		else if(!strcmp(*argv, "-h"))
		{
			/* print help */
			usage();
			goto error;
		}
		else if(!strcmp(*argv, "--verbose"))
		{
			/* enable verbose mode */
			is_verbose = 1;
		}
		else if(!strcmp(*argv, "-o"))
		{
			/* get the name of the file to store the ROHC packets */
			ofilename = argv[1];
			args_used++;
		}
		else if(!strcmp(*argv, "-c"))
		{
			/* get the name of the file where the ROHC packets used for comparison
			 * are stored */
			cmp_filename = argv[1];
			args_used++;
		}
		else if(!strcmp(*argv, "--rohc-size-output"))
		{
			/* get the name of the file to store the sizes of every ROHC packets */
			rohc_size_ofilename = argv[1];
			args_used++;
		}
		else if(!strcmp(*argv, "--max-contexts"))
		{
			/* get the maximum number of contexts the test should use */
			max_contexts = atoi(argv[1]);
			args_used++;
		}
		else if(cid_type == NULL)
		{
			/* get the type of CID to use within the ROHC library */
			cid_type = argv[0];
		}
		else if(src_filename == NULL)
		{
			/* get the name of the file that contains the packets to
			 * compress/decompress */
			src_filename = argv[0];
		}
		else
		{
			/* do not accept more than one filename without option name */
			usage();
			goto error;
		}
	}

	/* check CID type */
	if(!strcmp(cid_type, "smallcid"))
	{
		use_large_cid = 0;

		/* the maximum number of ROHC contexts should be valid */
		if(max_contexts < 1 || max_contexts > (ROHC_SMALL_CID_MAX + 1))
		{
			fprintf(stderr, "the maximum number of ROHC contexts should be "
			        "between 1 and %u\n\n", ROHC_SMALL_CID_MAX + 1);
			usage();
			goto error;
		}
	}
	else if(!strcmp(cid_type, "largecid"))
	{
		use_large_cid = 1;

		/* the maximum number of ROHC contexts should be valid */
		if(max_contexts < 1 || max_contexts > (ROHC_LARGE_CID_MAX + 1))
		{
			fprintf(stderr, "the maximum number of ROHC contexts should be "
			        "between 1 and %u\n\n", ROHC_LARGE_CID_MAX + 1);
			usage();
			goto error;
		}
	}
	else
	{
		fprintf(stderr, "invalid CID type '%s', only 'smallcid' and 'largecid' "
		        "expected\n", cid_type);
		goto error;
	}

	/* the source filename is mandatory */
	if(src_filename == NULL)
	{
		usage();
		goto error;
	}

	/* test ROHC compression/decompression with the packets from the file */
	status = test_comp_and_decomp(use_large_cid, max_contexts, src_filename,
	                              ofilename, cmp_filename, rohc_size_ofilename);

error:
	return status;
}


/**
 * @brief Print usage of the performance test application
 */
static void usage(void)
{
	fprintf(stderr,
	        "ROHC non-regression tool: test the ROHC library with a flow\n"
	        "                          of IP packets\n"
	        "\n"
	        "usage: test_non_regression [OPTIONS] CID_TYPE FLOW\n"
	        "\n"
	        "with:\n"
	        "  CID_TYPE                The type of CID to use among 'smallcid'\n"
	        "                          and 'largecid'\n"
	        "  FLOW                    The flow of Ethernet frames to compress\n"
	        "                          (in PCAP format)\n"
	        "\n"
	        "options:\n"
	        "  -v                      Print version information and exit\n"
	        "  -h                      Print this usage and exit\n"
	        "  -o FILE                 Save the generated ROHC packets in FILE\n"
	        "                          (PCAP format)\n"
	        "  -c FILE                 Compare the generated ROHC packets with the\n"
	        "                          ROHC packets stored in FILE (PCAP format)\n"
	        "  --rohc-size-output FILE  Save the sizes of ROHC packets in FILE\n"
	        "  --max-contexts NUM      The maximum number of ROHC contexts to\n"
	        "                          simultaneously use during the test\n"
	        "  --verbose               Run the test in verbose mode\n");
}


#if 0
/**
 * @brief Print statistics about the compressors and decompressors used during
 *        the test
 *
 * @param comp1   The first compressor
 * @param decomp1 The decompressor that receives data from the first compressor
 * @param comp2 The second compressor
 * @param decomp2 The decompressor that receives data from the second compressor
 */
static void show_rohc_stats(struct rohc_comp *comp1, struct rohc_decomp *decomp1,
                            struct rohc_comp *comp2, struct rohc_decomp *decomp2)
{
	char buffer[80000];
	int len;
	unsigned int indent = 2;

	buffer[0] = '\0';

	/* compute compressor statistics */
	len = rohc_c_statistics(comp1, indent, buffer);
	if(len < 0)
	{
		fprintf(stderr, "failed to compute statistics for compressor 1\n");
		goto error;
	}
	len = rohc_c_statistics(comp2, indent, buffer);
	if(len < 0)
	{
		fprintf(stderr, "failed to compute statistics for compressor 2\n");
		goto error;
	}

	/* compute decompressor statistics */
	len = rohc_d_statistics(decomp1, indent, buffer);
	if(len < 0)
	{
		fprintf(stderr, "failed to compute statistics for decompressor 1\n");
		goto error;
	}
	len = rohc_d_statistics(decomp2, indent, buffer);
	if(len < 0)
	{
		fprintf(stderr, "failed to compute statistics for decompressor 2\n");
		goto error;
	}

	/* print statistics */
	printf("%s", buffer);

error:
	return;
}
#endif


/**
 * @brief Compress and decompress one uncompressed IP packet with the given
 *        compressor and decompressor
 *
 * @param comp             The compressor to use to compress the IP packet
 * @param decomp           The decompressor to use to decompress the IP packet
 * @param num_comp         The ID of the compressor/decompressor
 * @param num_packet       A number affected to the IP packet to compress/decompress
 * @param header           The PCAP header for the packet
 * @param packet           The packet to compress/decompress (link layer included)
 * @param link_len_src     The length of the link layer header before IP data
 * @param use_large_cid    Whether use large CID or not
 * @param dumper           The PCAP output dump file
 * @param cmp_packet       The ROHC packet for comparison purpose
 * @param cmp_size         The size of the ROHC packet used for comparison
 *                         purpose
 * @param link_len_cmp     The length of the link layer header before ROHC data
 * @param size_output_file The name of the text file to output the sizes of
 *                         the ROHC packets
 * @param stats            The test stats
 * @return                 1 if the process is successful
 *                         0 if the decompressed packet doesn't match the
 *                         original one
 *                         -1 if an error occurs while compressing
 *                         -2 if an error occurs while decompressing
 *                         -3 if the link layer is not Ethernet
 */
static int compress_decompress(struct rohc_comp *comp,
                               struct rohc_decomp *decomp,
                               int num_comp,
                               int num_packet,
                               struct pcap_pkthdr header,
                               unsigned char *packet,
                               int link_len_src,
                               int use_large_cid,
                               pcap_dumper_t *dumper,
                               unsigned char *cmp_packet,
                               int cmp_size,
                               int link_len_cmp,
                               FILE *size_output_file,
                               struct test_stats *stats)
{
	unsigned char *ip_packet;
	size_t ip_size;
	static unsigned char output_packet[max(ETHER_HDR_LEN, LINUX_COOKED_HDR_LEN) + MAX_ROHC_SIZE];
	unsigned char *rohc_packet;
	size_t rohc_size;
	static unsigned char decomp_packet[MAX_ROHC_SIZE];
	int decomp_size;
	struct ether_header *eth_header;
	int ret = 1;
	rohc_comp_last_packet_info2_t last_packet_info;

	printf("\t<packet id=\"%d\" comp=\"%d\">\n", num_packet, num_comp);

	/* check Ethernet frame length */
	if(header.len <= link_len_src || header.len != header.caplen)
	{
		printf("\t\t<compression>\n");
		printf("\t\t\t<log>\n");
		printf("bad PCAP packet (len = %d, caplen = %d)\n", header.len, header.caplen);
		printf("\t\t\t</log>\n");
		printf("\t\t\t<status>failed</status>\n");
		printf("\t\t</compression>\n");
		printf("\n");
		printf("\t\t<decompression>\n");
		printf("\t\t\t<log>\n");
		printf("Compression failed, cannot decompress the ROHC packet!\n");
		printf("\t\t\t</log>\n");
		printf("\t\t\t<status>failed</status>\n");
		printf("\t\t</decompression>\n");
		printf("\n");
		printf("\t\t<comparison>\n");
		printf("\t\t\t<log>\n");
		printf("Compression failed, cannot compare the packets!\n");
		printf("\t\t\t</log>\n");
		printf("\t\t\t<status>failed</status>\n");
		printf("\t\t</comparison>\n");

		ret = -3;
		goto exit;
	}

	ip_packet = packet + link_len_src;
	ip_size = header.len - link_len_src;
	rohc_packet = output_packet + link_len_src;

	/* check for padding after the IP packet in the Ethernet payload */
	if(link_len_src == ETHER_HDR_LEN && header.len == ETHER_FRAME_MIN_LEN)
	{
		int version;
		size_t tot_len;

		version = (ip_packet[0] >> 4) & 0x0f;

		if(version == 4)
		{
			struct ipv4_hdr *ip = (struct ipv4_hdr *) ip_packet;
			tot_len = ntohs(ip->tot_len);
		}
		else
		{
			struct ipv6_hdr *ip = (struct ipv6_hdr *) ip_packet;
			tot_len = sizeof(struct ipv6_hdr) + ntohs(ip->ip6_plen);
		}

		if(tot_len < ip_size)
		{
			printf("The Ethernet frame has %zd bytes of padding after the "
			       "%zd byte IP packet!\n", ip_size - tot_len, tot_len);
			ip_size = tot_len;
		}
	}

	/* compress the IP packet */
	printf("\t\t<compression>\n");
	printf("\t\t\t<log>\n");
	ret = rohc_compress2(comp, ip_packet, ip_size,
	                     rohc_packet, MAX_ROHC_SIZE, &rohc_size);
	printf("\t\t\t</log>\n");
	if(ret != ROHC_OK)
	{
		printf("\t\t\t<status>failed</status>\n");
		printf("\t\t</compression>\n");
		printf("\n");
		printf("\t\t<rohc_comparison>\n");
		printf("\t\t\t<log>\n");
		printf("Compression failed, cannot compare the packets!\n");
		printf("\t\t\t</log>\n");
		printf("\t\t\t<status>failed</status>\n");
		printf("\t\t</rohc_comparison>\n");
		printf("\n");
		printf("\t\t<decompression>\n");
		printf("\t\t\t<log>\n");
		printf("Compression failed, cannot decompress the ROHC packet!\n");
		printf("\t\t\t</log>\n");
		printf("\t\t\t<status>failed</status>\n");
		printf("\t\t</decompression>\n");
		printf("\n");
		printf("\t\t<ip_comparison>\n");
		printf("\t\t\t<log>\n");
		printf("Compression failed, cannot compare the packets!\n");
		printf("\t\t\t</log>\n");
		printf("\t\t\t<status>failed</status>\n");
		printf("\t\t</ip_comparison>\n");

		ret = -1;
		goto exit;
	}

	printf("\t\t\t<status>ok</status>\n");
	printf("\t\t</compression>\n\n");

	/* output the ROHC packet to the PCAP dump file if asked */
	if(dumper != NULL)
	{
		header.len = link_len_src + rohc_size;
		header.caplen = header.len;
		if(link_len_src != 0)
		{
			memcpy(output_packet, packet, link_len_src); /* add the link layer header */
			if(link_len_src == ETHER_HDR_LEN) /* Ethernet only */
			{
				eth_header = (struct ether_header *) output_packet;
				eth_header->ether_type = htons(ROHC_ETHERTYPE); /* ROHC Ethertype */
			}
			else if(link_len_src == LINUX_COOKED_HDR_LEN) /* Linux Cooked Sockets only */
			{
				output_packet[LINUX_COOKED_HDR_LEN - 2] = ROHC_ETHERTYPE & 0xff;
				output_packet[LINUX_COOKED_HDR_LEN - 1] = (ROHC_ETHERTYPE >> 8) & 0xff;
			}
		}
		pcap_dump((u_char *) dumper, &header, output_packet);
	}

	/* get some statistics about the last compressed packet */
	last_packet_info.version_major = 0;
	last_packet_info.version_minor = 0;
	if(!rohc_comp_get_last_packet_info2(comp, &last_packet_info))
	{
		printf("\n");
		printf("\t\t<rohc_comparison>\n");
		printf("\t\t\t<log>\n");
		printf("Getting statistics failed, cannot compare the packets!\n");
		printf("\t\t\t</log>\n");
		printf("\t\t\t<status>failed</status>\n");
		printf("\t\t</rohc_comparison>\n");
		printf("\n");
		printf("\t\t<decompression>\n");
		printf("\t\t\t<log>\n");
		printf("Compression failed, cannot decompress the ROHC packet!\n");
		printf("\t\t\t</log>\n");
		printf("\t\t\t<status>failed</status>\n");
		printf("\t\t</decompression>\n");
		printf("\n");
		printf("\t\t<ip_comparison>\n");
		printf("\t\t\t<log>\n");
		printf("Compression failed, cannot compare the packets!\n");
		printf("\t\t\t</log>\n");
		printf("\t\t\t<status>failed</status>\n");
		printf("\t\t</ip_comparison>\n");

		ret = -1;
		goto exit;
	}
	stats->comp_pre += ip_size;
	stats->comp_post += rohc_size;
	stats->comp_nr_pkts_per_profile[last_packet_info.profile_id]++;
	stats->comp_nr_pkts_per_mode[last_packet_info.context_mode]++;
	stats->comp_nr_pkts_per_state[last_packet_info.context_state]++;
	stats->comp_nr_pkts_per_pkt_type[last_packet_info.packet_type]++;
	if(last_packet_info.is_context_init)
	{
		stats->comp_nr_reused_cid++;
	}

	/* output the size of the ROHC packet to the output file if asked */
	if(size_output_file != NULL)
	{
		fprintf(size_output_file, "compressor_num = %d\tpacket_num = %d\t"
		        "rohc_size = %zd\tpacket_type = %d\n", num_comp, num_packet,
		        rohc_size, last_packet_info.packet_type);
	}

	/* compare the ROHC packets with the ones given by the user if asked */
	printf("\t\t<rohc_comparison>\n");
	printf("\t\t\t<log>\n");
#if defined(RTP_BIT_TYPE) && RTP_BIT_TYPE
	printf("RTP bit type option enabled, comparison with ROHC packets "
	       "of reference is skipped because they will not match\n");
	printf("\t\t\t</log>\n");
	printf("\t\t\t<status>failed</status>\n");
	ret = 0;
#else
	if(cmp_packet != NULL && cmp_size > link_len_cmp)
	{
		if(!compare_packets(cmp_packet + link_len_cmp, cmp_size - link_len_cmp,
		                    rohc_packet, rohc_size))
		{
			printf("\t\t\t</log>\n");
			printf("\t\t\t<status>failed</status>\n");
			ret = 0;
		}
		else
		{
			printf("Packets are equal\n");
			printf("\t\t\t</log>\n");
			printf("\t\t\t<status>ok</status>\n");
		}
	}
	else
	{
		printf("No ROHC packets given for reference, cannot compare (run with the -c option)\n");
		printf("\t\t\t</log>\n");
		printf("\t\t\t<status>failed</status>\n");
		ret = 0;
	}
#endif
	printf("\t\t</rohc_comparison>\n\n");

	/* decompress the ROHC packet */
	printf("\t\t<decompression>\n");
	printf("\t\t\t<log>\n");
	decomp_size = rohc_decompress(decomp,
	                              rohc_packet, rohc_size,
	                              decomp_packet, MAX_ROHC_SIZE);
	printf("\t\t\t</log>\n");

	if(decomp_size <= 0)
	{
		size_t i;

		printf("\t\t\t<status>failed</status>\n");
		printf("\t\t</decompression>\n");
		printf("\n");
		printf("\t\t<ip_comparison>\n");
		printf("\t\t\t<log>\n");
		printf("Decompression failed, cannot compare the packets!\n");
		printf("Original %zd-byte non-compressed packet:\n", ip_size);
		for(i = 0; i < ip_size; i++)
		{
			if(i > 0 && (i % 16) == 0)
			{
				printf("\n");
			}
			else if(i > 0 && (i % 8) == 0)
			{
				printf("  ");
			}
			printf("%02x ", ip_packet[i]);
		}
		printf("\n");
		printf("\t\t\t</log>\n");
		printf("\t\t\t<status>failed</status>\n");
		printf("\t\t</ip_comparison>\n");

		ret = -2;
		goto exit;
	}

	printf("\t\t\t<status>ok</status>\n");
	printf("\t\t</decompression>\n\n");

	/* compare the decompressed packet with the original one */
	printf("\t\t<ip_comparison>\n");
	printf("\t\t\t<log>\n");
	if(!compare_packets(ip_packet, ip_size, decomp_packet, decomp_size))
	{
		printf("\t\t\t</log>\n");
		printf("\t\t\t<status>failed</status>\n");
		ret = 0;
	}
	else
	{
		printf("Packets are equal\n");
		printf("\t\t\t</log>\n");
		printf("\t\t\t<status>ok</status>\n");
	}
	printf("\t\t</ip_comparison>\n");

exit:
	printf("\t</packet>\n\n");
	return ret;
}


/**
 * @brief Test the ROHC library with a flow of IP packets going through
 *        two compressor/decompressor pairs
 *
 * @param use_large_cid        Whether the compressor shall use large CIDs
 * @param max_contexts         The maximum number of ROHC contexts to use
 * @param src_filename         The name of the PCAP file that contains the
 *                             IP packets
 * @param ofilename            The name of the PCAP file to output the ROHC
 *                             packets
 * @param cmp_filename         The name of the PCAP file that contains the
 *                             ROHC packets used for comparison
 * @param rohc_size_ofilename  The name of the text file to output the sizes
 *                             of the ROHC packets
 * @return                     0 in case of success,
 *                             1 in case of failure,
 *                             77 if test is skipped
 */
static int test_comp_and_decomp(const int use_large_cid,
                                const unsigned int max_contexts,
                                char *src_filename,
                                char *ofilename,
                                char *cmp_filename,
                                const char *rohc_size_ofilename)
{
	char errbuf[PCAP_ERRBUF_SIZE];
	pcap_t *handle;
	pcap_t *cmp_handle;
	pcap_dumper_t *dumper;
	int link_layer_type_src, link_layer_type_cmp;
	int link_len_src, link_len_cmp = 0;
	struct pcap_pkthdr header;
	struct pcap_pkthdr cmp_header;

	FILE *rohc_size_output_file;

	unsigned char *packet;
	unsigned char *cmp_packet;

	int counter;

	struct rohc_comp *comp1;
	struct rohc_comp *comp2;

	struct rohc_decomp *decomp1;
	struct rohc_decomp *decomp2;

#if 0
#define NB_RTP_PORTS 5
	const unsigned int rtp_ports[NB_RTP_PORTS] =
		{ 1234, 36780, 33238, 5020, 5002 };
#endif
	int i;

	int ret;
	int nb_bad = 0, nb_ok = 0, err_comp = 0, err_decomp = 0, nb_ref = 0;
	int status = 1;

	struct test_stats stats1;
	struct test_stats stats2;

	printf("<?xml version=\"1.0\" encoding=\"ISO-8859-15\"?>\n");
	printf("<test>\n");
	printf("\t<startup>\n");
	printf("\t\t<log>\n");

	/* reset stats */
	memset(&stats1, 0, sizeof(struct test_stats));
	memset(&stats2, 0, sizeof(struct test_stats));

	/* open the source dump file */
	handle = pcap_open_offline(src_filename, errbuf);
	if(handle == NULL)
	{
		printf("failed to open the source pcap file: %s\n", errbuf);
		printf("\t\t</log>\n");
		printf("\t\t<status>failed</status>\n");
		printf("\t</startup>\n\n");
		goto error;
	}

	/* link layer in the source dump must be Ethernet */
	link_layer_type_src = pcap_datalink(handle);
	if(link_layer_type_src != DLT_EN10MB &&
	   link_layer_type_src != DLT_LINUX_SLL &&
	   link_layer_type_src != DLT_RAW)
	{
		printf("link layer type %d not supported in source dump (supported = "
		       "%d, %d, %d)\n", link_layer_type_src, DLT_EN10MB, DLT_LINUX_SLL,
		       DLT_RAW);
		printf("\t\t</log>\n");
		printf("\t\t<status>failed</status>\n");
		printf("\t</startup>\n\n");
		goto close_input;
	}

	if(link_layer_type_src == DLT_EN10MB)
	{
		link_len_src = ETHER_HDR_LEN;
	}
	else if(link_layer_type_src == DLT_LINUX_SLL)
	{
		link_len_src = LINUX_COOKED_HDR_LEN;
	}
	else /* DLT_RAW */
	{
		link_len_src = 0;
	}

	/* open the network dump file for ROHC storage if asked */
	if(ofilename != NULL)
	{
		dumper = pcap_dump_open(handle, ofilename);
		if(dumper == NULL)
		{
			printf("failed to open dump file: %s\n", errbuf);
			printf("\t\t</log>\n");
			printf("\t\t<status>failed</status>\n");
			printf("\t</startup>\n\n");
			goto close_input;
		}
	}
	else
	{
		dumper = NULL;
	}

	/* open the ROHC comparison dump file if asked */
	if(cmp_filename != NULL)
	{
		cmp_handle = pcap_open_offline(cmp_filename, errbuf);
		if(cmp_handle == NULL)
		{
			printf("failed to open the comparison pcap file: %s\n", errbuf);
			printf("\t\t</log>\n");
			printf("\t\t<status>failed</status>\n");
			printf("\t</startup>\n\n");
			goto close_output;
		}

		/* link layer in the rohc_comparison dump must be Ethernet */
		link_layer_type_cmp = pcap_datalink(cmp_handle);
		if(link_layer_type_cmp != DLT_EN10MB &&
		   link_layer_type_cmp != DLT_LINUX_SLL &&
		   link_layer_type_cmp != DLT_RAW)
		{
			printf("link layer type %d not supported in comparision dump "
			       "(supported = %d, %d, %d)\n", link_layer_type_cmp, DLT_EN10MB,
			       DLT_LINUX_SLL, DLT_RAW);
			printf("\t\t</log>\n");
			printf("\t\t<status>failed</status>\n");
			printf("\t</startup>\n\n");
			goto close_comparison;
		}

		if(link_layer_type_cmp == DLT_EN10MB)
		{
			link_len_cmp = ETHER_HDR_LEN;
		}
		else if(link_layer_type_cmp == DLT_LINUX_SLL)
		{
			link_len_cmp = LINUX_COOKED_HDR_LEN;
		}
		else /* DLT_RAW */
		{
			link_len_cmp = 0;
		}
	}
	else
	{
		cmp_handle = NULL;
	}

	/* open the file in which to write the sizes of the ROHC packets if asked */
	if(rohc_size_ofilename != NULL)
	{
		rohc_size_output_file = fopen(rohc_size_ofilename, "w+");
		if(rohc_size_output_file == NULL)
		{
			printf("failed to open file '%s' to output the sizes of ROHC packets: "
			       "%s (%d)\n", rohc_size_ofilename, strerror(errno), errno);
			printf("\t\t</log>\n");
			printf("\t\t<status>failed</status>\n");
			printf("\t</startup>\n\n");
			goto close_comparison;
		}
	}
	else
	{
		rohc_size_output_file = NULL;
	}

	/* create the compressor 1 */
	comp1 = rohc_alloc_compressor(max_contexts - 1, 0, 0, 0);
	if(comp1 == NULL)
	{
		printf("cannot create the compressor 1\n");
		printf("\t\t</log>\n");
		printf("\t\t<status>failed</status>\n");
		printf("\t</startup>\n\n");
		goto close_output_size;
	}

	/* set the callback for traces on compressor 1 */
	if(!rohc_comp_set_traces_cb(comp1, print_rohc_traces))
	{
		fprintf(stderr, "failed to set the callback for traces on "
		        "compressor 1\n");
		printf("\t\t</log>\n");
		printf("\t\t<status>failed</status>\n");
		printf("\t</startup>\n\n");
		printf("\t<shutdown>\n");
		printf("\t\t<log>\n");
		goto destroy_comp1;
	}

	/* enable profiles */
	rohc_activate_profile(comp1, ROHC_PROFILE_UNCOMPRESSED);
	rohc_activate_profile(comp1, ROHC_PROFILE_UDP);
	rohc_activate_profile(comp1, ROHC_PROFILE_IP);
	rohc_activate_profile(comp1, ROHC_PROFILE_UDPLITE);
	rohc_activate_profile(comp1, ROHC_PROFILE_RTP);
	rohc_activate_profile(comp1, ROHC_PROFILE_ESP);
	rohc_c_set_large_cid(comp1, use_large_cid);

	/* set the callback for random numbers on compressor 1 */
	if(!rohc_comp_set_random_cb(comp1, gen_false_random_num, NULL))
	{
		fprintf(stderr, "failed to set the callback for random numbers on "
		        "compressor 1\n");
		printf("\t\t</log>\n");
		printf("\t\t<status>failed</status>\n");
		printf("\t</startup>\n\n");
		printf("\t<shutdown>\n");
		printf("\t\t<log>\n");
		goto destroy_comp1;
	}

	assert(rohc_comp_set_wlsb_window_width(comp1, 8) == true);
	assert(rohc_comp_set_periodic_refreshes(comp1, 400, 150) == true);

	/* reset list of RTP ports for compressor 1 */
	if(!rohc_comp_reset_rtp_ports(comp1))
	{
		fprintf(stderr, "failed to reset list of RTP ports for compressor 1\n");
		printf("\t\t</log>\n");
		printf("\t\t<status>failed</status>\n");
		printf("\t</startup>\n\n");
		printf("\t<shutdown>\n");
		printf("\t\t<log>\n");
		goto destroy_comp1;
	}

#if 0
	/* add some ports to the list of RTP ports */
	for(i = 0; i < NB_RTP_PORTS; i++)
	{
		if(!rohc_comp_add_rtp_port(comp1, rtp_ports[i]))
		{
			printf("failed to enable RTP port %u for compressor 1\n",
			       rtp_ports[i]);
			printf("\t\t</log>\n");
			printf("\t\t<status>failed</status>\n");
			printf("\t</startup>\n\n");
			printf("\t<shutdown>\n");
			printf("\t\t<log>\n");
			goto destroy_comp1;
		}
	}
#else
	/* set the callback for RTP stream detection */
	if(!rohc_comp_set_rtp_detection_cb(comp1, rtp_detect_cb, NULL))
	{
		fprintf(stderr, "failed to set the RTP stream detection callback for "
		        "compressor 1\n");
		goto destroy_comp1;
	}
#endif

	/* create the compressor 2 */
	comp2 = rohc_alloc_compressor(max_contexts - 1, 0, 0, 0);
	if(comp2 == NULL)
	{
		printf("cannot create the compressor 2\n");
		printf("\t\t</log>\n");
		printf("\t\t<status>failed</status>\n");
		printf("\t</startup>\n\n");
		printf("\t<shutdown>\n");
		printf("\t\t<log>\n");
		goto destroy_comp1;
	}

	/* set the callback for traces on compressor 2 */
	if(!rohc_comp_set_traces_cb(comp2, print_rohc_traces))
	{
		fprintf(stderr, "failed to set the callback for traces on "
		        "compressor 2\n");
		printf("\t\t</log>\n");
		printf("\t\t<status>failed</status>\n");
		printf("\t</startup>\n\n");
		printf("\t<shutdown>\n");
		printf("\t\t<log>\n");
		goto destroy_comp2;
	}

	/* enable profiles */
	rohc_activate_profile(comp2, ROHC_PROFILE_UNCOMPRESSED);
	rohc_activate_profile(comp2, ROHC_PROFILE_UDP);
	rohc_activate_profile(comp2, ROHC_PROFILE_IP);
	rohc_activate_profile(comp2, ROHC_PROFILE_UDPLITE);
	rohc_activate_profile(comp2, ROHC_PROFILE_RTP);
	rohc_activate_profile(comp2, ROHC_PROFILE_ESP);
	rohc_c_set_large_cid(comp2, use_large_cid);

	/* set the callback for random numbers on compressor 2 */
	if(!rohc_comp_set_random_cb(comp2, gen_false_random_num, NULL))
	{
		fprintf(stderr, "failed to set the callback for random numbers on "
		        "compressor 2\n");
		printf("\t\t</log>\n");
		printf("\t\t<status>failed</status>\n");
		printf("\t</startup>\n\n");
		printf("\t<shutdown>\n");
		printf("\t\t<log>\n");
		goto destroy_comp2;
	}

	assert(rohc_comp_set_wlsb_window_width(comp2, 8) == true);
	assert(rohc_comp_set_periodic_refreshes(comp2, 400, 150) == true);

	/* reset list of RTP ports for compressor 2 */
	if(!rohc_comp_reset_rtp_ports(comp2))
	{
		fprintf(stderr, "failed to reset list of RTP ports for compressor 2\n");
		printf("\t\t</log>\n");
		printf("\t\t<status>failed</status>\n");
		printf("\t</startup>\n\n");
		printf("\t<shutdown>\n");
		printf("\t\t<log>\n");
		goto destroy_comp2;
	}

#if 0
	/* add some ports to the list of RTP ports */
	for(i = 0; i < NB_RTP_PORTS; i++)
	{
		if(!rohc_comp_add_rtp_port(comp2, rtp_ports[i]))
		{
			printf("failed to enable RTP port %u for compressor 2\n",
			       rtp_ports[i]);
			printf("\t\t</log>\n");
			printf("\t\t<status>failed</status>\n");
			printf("\t</startup>\n\n");
			goto destroy_comp2;
		}
	}
#else
	/* set the callback for RTP stream detection */
	if(!rohc_comp_set_rtp_detection_cb(comp2, rtp_detect_cb, NULL))
	{
		fprintf(stderr, "failed to set the RTP stream detection callback for "
		        "compressor 2\n");
		goto destroy_comp2;
	}
#endif

	/* create the decompressor 1 */
	decomp1 = rohc_alloc_decompressor(comp2);
	if(decomp1 == NULL)
	{
		printf("cannot create the decompressor 1\n");
		printf("\t\t</log>\n");
		printf("\t\t<status>failed</status>\n");
		printf("\t</startup>\n\n");
		printf("\t<shutdown>\n");
		printf("\t\t<log>\n");
		goto destroy_comp2;
	}

	/* set the callback for traces on decompressor 1 */
	if(!rohc_decomp_set_traces_cb(decomp1, print_rohc_traces))
	{
		printf("cannot set trace callback for decompressor 1\n");
		printf("\t\t</log>\n");
		printf("\t\t<status>failed</status>\n");
		printf("\t</startup>\n\n");
		printf("\t<shutdown>\n");
		printf("\t\t<log>\n");
		goto destroy_decomp1;
	}

	/* set CID type and MAX_CID for decompressor 1 */
	if(use_large_cid)
	{
		if(!rohc_decomp_set_cid_type(decomp1, ROHC_LARGE_CID))
		{
			fprintf(stderr, "failed to set CID type to large CIDs for "
			        "decompressor 1\n");
			goto destroy_decomp1;
		}
		if(!rohc_decomp_set_max_cid(decomp1, ROHC_LARGE_CID_MAX))
		{
			fprintf(stderr, "failed to set MAX_CID to %d for "
			        "decompressor 1\n", ROHC_LARGE_CID_MAX);
			goto destroy_decomp1;
		}
	}
	else
	{
		if(!rohc_decomp_set_cid_type(decomp1, ROHC_SMALL_CID))
		{
			fprintf(stderr, "failed to set CID type to small CIDs for "
			        "decompressor 1\n");
			goto destroy_decomp1;
		}
		if(!rohc_decomp_set_max_cid(decomp1, ROHC_SMALL_CID_MAX))
		{
			fprintf(stderr, "failed to set MAX_CID to %d for "
			        "decompressor 1\n", ROHC_SMALL_CID_MAX);
			goto destroy_decomp1;
		}
	}

	/* create the decompressor 2 */
	decomp2 = rohc_alloc_decompressor(comp1);
	if(decomp2 == NULL)
	{
		printf("cannot create the decompressor 2\n");
		printf("\t\t</log>\n");
		printf("\t\t<status>failed</status>\n");
		printf("\t</startup>\n\n");
		printf("\t<shutdown>\n");
		printf("\t\t<log>\n");
		goto destroy_decomp1;
	}

	/* set the callback for traces on decompressor 2 */
	if(!rohc_decomp_set_traces_cb(decomp2, print_rohc_traces))
	{
		printf("cannot set trace callback for decompressor 2\n");
		printf("\t\t</log>\n");
		printf("\t\t<status>failed</status>\n");
		printf("\t</startup>\n\n");
		printf("\t<shutdown>\n");
		printf("\t\t<log>\n");
		goto destroy_decomp1;
	}

	/* set CID type and MAX_CID for decompressor 2 */
	if(use_large_cid)
	{
		if(!rohc_decomp_set_cid_type(decomp2, ROHC_LARGE_CID))
		{
			fprintf(stderr, "failed to set CID type to large CIDs for "
			        "decompressor 2\n");
			goto destroy_decomp2;
		}
		if(!rohc_decomp_set_max_cid(decomp2, ROHC_LARGE_CID_MAX))
		{
			fprintf(stderr, "failed to set MAX_CID to %d for "
			        "decompressor 2\n", ROHC_LARGE_CID_MAX);
			goto destroy_decomp2;
		}
	}
	else
	{
		if(!rohc_decomp_set_cid_type(decomp2, ROHC_SMALL_CID))
		{
			fprintf(stderr, "failed to set CID type to small CIDs for "
			        "decompressor 2\n");
			goto destroy_decomp2;
		}
		if(!rohc_decomp_set_max_cid(decomp2, ROHC_SMALL_CID_MAX))
		{
			fprintf(stderr, "failed to set MAX_CID to %d for "
			        "decompressor 2\n", ROHC_SMALL_CID_MAX);
			goto destroy_decomp2;
		}
	}

	printf("\t\t</log>\n");
	printf("\t\t<status>ok</status>\n");
	printf("\t</startup>\n\n");

	/* for each packet in the dump */
	counter = 0;
	while((packet = (unsigned char *) pcap_next(handle, &header)) != NULL)
	{
		counter++;

		/* get next ROHC packet from the comparison dump file if asked */
		if(cmp_handle != NULL)
		{
			cmp_packet = (unsigned char *) pcap_next(cmp_handle, &cmp_header);
		}
		else
		{
			cmp_packet = NULL;
		}

		/* compress & decompress from compressor 1 to decompressor 1 */
		ret = compress_decompress(comp1, decomp1, 1, counter, header, packet,
		                          link_len_src, use_large_cid,
		                          dumper, cmp_packet,
		                          cmp_header.caplen, link_len_cmp,
		                          rohc_size_output_file, &stats1);
		if(ret == -1)
		{
			err_comp++;
			break;
		}
		else if(ret == -2)
		{
			err_decomp++;
			break;
		}
		else if(ret == 0)
		{
			nb_ref++;
		}
		else if(ret == 1)
		{
			nb_ok++;
		}
		else
		{
			nb_bad++;
		}

		/* get next ROHC packet from the comparison dump file if asked */
		if(cmp_handle != NULL)
		{
			cmp_packet = (unsigned char *) pcap_next(cmp_handle, &cmp_header);
		}
		else
		{
			cmp_packet = NULL;
		}

		/* compress & decompress from compressor 2 to decompressor 2 */
		ret = compress_decompress(comp2, decomp2, 2, counter, header, packet,
		                          link_len_src, use_large_cid,
		                          dumper, cmp_packet,
		                          cmp_header.caplen, link_len_cmp,
		                          rohc_size_output_file, &stats2);
		if(ret == -1)
		{
			err_comp++;
			break;
		}
		else if(ret == -2)
		{
			err_decomp++;
			break;
		}
		else if(ret == 0)
		{
			nb_ref++;
		}
		else if(ret == 1)
		{
			nb_ok++;
		}
		else
		{
			nb_bad++;
		}
	}

	/* show the compression/decompression results */
	printf("\t<summary>\n");
	printf("\t\t<packets_processed>%d</packets_processed>\n", 2 * counter);
	printf("\t\t<compression_failed>%d</compression_failed>\n",  nb_bad + err_comp);
	printf("\t\t<decompression_failed>%d</decompression_failed>\n", err_decomp);
	printf("\t\t<matches>%d</matches>\n", nb_ok);
	printf("\t</summary>\n\n");

	/* show some info/stats about the compressors and decompressors */
	printf("\t<infos>\n");
//	show_rohc_stats(comp1, decomp1, comp2, decomp2);

	printf("compressor #1:\n");
	printf("\tgeneral:\n");
	printf("\t\tpre-compress bytes %lu\n", stats1.comp_pre);
	printf("\t\tpost-compress bytes %lu\n", stats1.comp_post);
	if(stats1.comp_pre != 0)
	{
		printf("\t\tcompress ratio %lu\n", stats1.comp_post * 100 /
		       stats1.comp_pre);
	}
	else
	{
		printf("\t\tcompress ratio 0\n");
	}

	/* packets per profile */
	printf("\tpackets per profile:\n");
	printf("\t\tUncompressed profile packets %lu\n",
	       stats1.comp_nr_pkts_per_profile[ROHC_PROFILE_UNCOMPRESSED]);
	printf("\t\tIP/UDP/RTP profile packets %lu\n",
	       stats1.comp_nr_pkts_per_profile[ROHC_PROFILE_RTP]);
	printf("\t\tIP/UDP profile packets %lu\n",
	       stats1.comp_nr_pkts_per_profile[ROHC_PROFILE_UDP]);
	printf("\t\tIP-only profile packets %lu\n",
	       stats1.comp_nr_pkts_per_profile[ROHC_PROFILE_IP]);
	printf("\t\tIP/UDP-Lite profile packets %lu\n",
	       stats1.comp_nr_pkts_per_profile[ROHC_PROFILE_UDPLITE]);

	/* packets per mode */
	printf("\tpackets per mode:\n");
	printf("\t\tU-mode packets %lu\n",
	       stats1.comp_nr_pkts_per_mode[U_MODE]);
	printf("\t\tO-mode packets %lu\n",
	       stats1.comp_nr_pkts_per_mode[O_MODE]);
	printf("\t\tR-mode packets %lu\n",
	       stats1.comp_nr_pkts_per_mode[R_MODE]);

	/* packets per state */
	printf("\tpackets per state:\n");
	printf("\t\tIR state packets %lu\n",
	       stats1.comp_nr_pkts_per_state[IR]);
	printf("\t\tFO state packets %lu\n",
	       stats1.comp_nr_pkts_per_state[FO]);
	printf("\t\tSO state packets %lu\n",
	       stats1.comp_nr_pkts_per_state[SO]);

	/* packets per packet type */
	printf("\tpackets per packet type:\n");
	for(i = PACKET_IR; i < PACKET_UNKNOWN; i++)
	{
		printf("\t\tpacket type %s packets %lu\n", rohc_get_packet_descr(i),
		       stats1.comp_nr_pkts_per_pkt_type[i]);
	}

	/* re-used contexts */
	printf("\tre-used contexts count %lu\n", stats1.comp_nr_reused_cid);


	printf("\t</infos>\n\n");

	/* destroy the compressors and decompressors */
	printf("\t<shutdown>\n");
	printf("\t\t<log>\n\n");

#if defined(RTP_BIT_TYPE) && RTP_BIT_TYPE
	if(err_comp == 0 && err_decomp == 0 &&
	   nb_bad == 0 && nb_ref == (counter * 2) &&
	   nb_ok == 0)
	{
		/* test is successful, but exit with code 77 to report test as skipped
		   because of the RTP bit type option */
		status = 77;
	}
#else
	if(err_comp == 0 && err_decomp == 0 &&
	   nb_bad == 0 && nb_ref == 0 &&
	   nb_ok == (counter * 2))
	{
		/* test is successful */
		status = 0;
	}
#endif

destroy_decomp2:
	rohc_free_decompressor(decomp2);
destroy_decomp1:
	rohc_free_decompressor(decomp1);
destroy_comp2:
	rohc_free_compressor(comp2);
destroy_comp1:
	rohc_free_compressor(comp1);
	printf("\t\t</log>\n");
	printf("\t\t<status>ok</status>\n");
	printf("\t</shutdown>\n\n");
close_output_size:
	if(rohc_size_output_file != NULL)
	{
		fclose(rohc_size_output_file);
	}
close_comparison:
	if(cmp_handle != NULL)
	{
		pcap_close(cmp_handle);
	}
close_output:
	if(dumper != NULL)
	{
		pcap_dump_close(dumper);
	}
close_input:
	pcap_close(handle);
error:
	printf("</test>\n");
	return status;
}


/**
 * @brief Callback to print traces of the ROHC library
 *
 * @param level    The priority level of the trace
 * @param entity   The entity that emitted the trace among:
 *                  \li ROHC_TRACE_COMP
 *                  \li ROHC_TRACE_DECOMP
 * @param profile  The ID of the ROHC compression/decompression profile
 *                 the trace is related to
 * @param format   The format string of the trace
 */
static void print_rohc_traces(const rohc_trace_level_t level,
                              const rohc_trace_entity_t entity,
                              const int profile,
                              const char *const format,
                              ...)
{
	const char *level_descrs[] =
	{
		[ROHC_TRACE_DEBUG]   = "DEBUG",
		[ROHC_TRACE_INFO]    = "INFO",
		[ROHC_TRACE_WARNING] = "WARNING",
		[ROHC_TRACE_ERROR]   = "ERROR"
	};

	if(level >= ROHC_TRACE_WARNING || is_verbose)
	{
		va_list args;
		fprintf(stdout, "[%s] ", level_descrs[level]);
		va_start(args, format);
		vfprintf(stdout, format, args);
		va_end(args);
	}
}


/**
 * @brief Generate a false random number for testing the ROHC library
 *
 * @param comp          The ROHC compressor
 * @param user_context  Should always be NULL
 * @return              Always 0
 */
static int gen_false_random_num(const struct rohc_comp *const comp,
                                void *const user_context)
{
	assert(comp != NULL);
	assert(user_context == NULL);
	return 0;
}


/**
 * @brief Compare two network packets and print differences if any
 *
 * @param pkt1      The first packet
 * @param pkt1_size The size of the first packet
 * @param pkt2      The second packet
 * @param pkt2_size The size of the second packet
 * @return          Whether the packets are equal or not
 */
static int compare_packets(unsigned char *pkt1, int pkt1_size,
                           unsigned char *pkt2, int pkt2_size)
{
	int valid = 1;
	int min_size;
	int i, j, k;
	char str1[4][7], str2[4][7];
	char sep1, sep2;

	/* do not compare more than the shortest of the 2 packets */
	min_size = min(pkt1_size, pkt2_size);

	/* do not compare more than 180 bytes to avoid huge output */
	min_size = min(180, min_size);

	/* if packets are equal, do not print the packets */
	if(pkt1_size == pkt2_size && memcmp(pkt1, pkt2, pkt1_size) == 0)
	{
		goto skip;
	}

	/* packets are different */
	valid = 0;

	printf("------------------------------ Compare ------------------------------\n");
	printf("--------- reference ----------         ----------- new --------------\n");

	if(pkt1_size != pkt2_size)
	{
		printf("packets have different sizes (%d != %d), compare only the %d "
		       "first bytes\n", pkt1_size, pkt2_size, min_size);
	}

	j = 0;
	for(i = 0; i < min_size; i++)
	{
		if(pkt1[i] != pkt2[i])
		{
			sep1 = '#';
			sep2 = '#';
		}
		else
		{
			sep1 = '[';
			sep2 = ']';
		}

		sprintf(str1[j], "%c0x%.2x%c", sep1, pkt1[i], sep2);
		sprintf(str2[j], "%c0x%.2x%c", sep1, pkt2[i], sep2);

		/* make the output human readable */
		if(j >= 3 || (i + 1) >= min_size)
		{
			for(k = 0; k < 4; k++)
			{
				if(k < (j + 1))
				{
					printf("%s  ", str1[k]);
				}
				else /* fill the line with blanks if nothing to print */
				{
					printf("        ");
				}
			}

			printf("       ");

			for(k = 0; k < (j + 1); k++)
			{
				printf("%s  ", str2[k]);
			}

			printf("\n");

			j = 0;
		}
		else
		{
			j++;
		}
	}

	printf("----------------------- packets are different -----------------------\n");

skip:
	return valid;
}


/**
 * @brief The detection callback which do detect RTP stream
 *
 * @param ip           The inner ip packet
 * @param udp          The udp header of the packet
 * @param payload      The payload of the packet
 * @param payload_size The size of the payload (in bytes)
 * @return             1 if the packet is an RTP packet, 0 otherwise
 */
static bool rtp_detect_cb(const unsigned char *const ip,
                          const unsigned char *const udp,
                          const unsigned char *const payload,
                          const unsigned int payload_size,
                          void *const rtp_private)
{
	const uint16_t max_well_known_port = 1024;
	const uint16_t sip_port = 5060;
	uint16_t udp_sport;
	uint16_t udp_dport;
	uint16_t udp_len;
	uint8_t rtp_pt;
	bool is_rtp = false;

	assert(ip != NULL);
	assert(udp != NULL);
	assert(payload != NULL);
	assert(rtp_private == NULL);

	/* retrieve UDP source and destination ports and UDP length */
	memcpy(&udp_sport, udp, sizeof(uint16_t));
	memcpy(&udp_dport, udp + 2, sizeof(uint16_t));
	memcpy(&udp_len, udp + 4, sizeof(uint16_t));

	/* RTP streams do not use well known ports */
	if(ntohs(udp_sport) <= max_well_known_port ||
	   ntohs(udp_dport) <= max_well_known_port)
	{
		goto not_rtp;
	}

	/* SIP (UDP/5060) is not RTP */
	if(ntohs(udp_sport) == sip_port && ntohs(udp_dport) == sip_port)
	{
		goto not_rtp;
	}

	/* the UDP destination port of RTP packet is even (the RTCP destination
	 * port are RTP destination port + 1, so it is odd) */
	if((ntohs(udp_sport) % 2) != 0 || (ntohs(udp_dport) % 2) != 0)
	{
		goto not_rtp;
	}

	/* UDP Length shall not be too large */
	if(ntohs(udp_len) > 200)
	{
		goto not_rtp;
	}

	/* UDP payload shall at least contain the smallest RTP header */
	if(payload_size < 12)
	{
		goto not_rtp;
	}

	/* RTP version bits shall be 2 */
	if(((payload[0] >> 6) & 0x3) != 0x2)
	{
		goto not_rtp;
	}

	/* RTP payload type shall be GSM (0x03), ITU-T G.723 (0x04),
	 * ITU-T G.729 (0x12), dynamic RTP type 97 (0x61), or
	 * telephony-event (0x65) */
	rtp_pt = payload[1] & 0x7f;
	if(rtp_pt != 0x03 && rtp_pt != 0x04 && rtp_pt != 0x12 &&
	   rtp_pt != 0x61 && rtp_pt != 0x65)
	{
		goto not_rtp;
	}

	/* we think that the UDP packet is a RTP packet */
	is_rtp = true;

not_rtp:
	return is_rtp;
}

