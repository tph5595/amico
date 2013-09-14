/**
 ***************************************************************************
 * Copyright (C) 2011, Roberto Perdisci                                    *
 * perdisci@cs.uga.edu                                                     *
 *                                                                         *
 * Distributed under the GNU Public License                                *
 * http://www.gnu.org/licenses/gpl.txt                                     *   
 *                                                                         *
 * This program is free software; you can redistribute it and/or modify    *
 * it under the terms of the GNU General Public License as published by    *
 * the Free Software Foundation; either version 2 of the License, or       *
 * (at your option) any later version.                                     *
 *                                                                         *
 ***************************************************************************
 */


#include <pcap.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include "lru-cache.h"
#include "seq_list.h"

#define MIN(a,b) (((a)<(b))?(a):(b))
#define MAX(a,b) (((a)>(b))?(a):(b))

#define ETH_ADDR_LEN  6
#define ETH_HEADER_LEN 14
#define PCAP_SNAPLEN 1514
// #define PCAP_SNAPLEN 1600 // increased to cover corener cases with eth.len > 1514
// #define PCAP_SNAPLEN 65535 // increased to cover jumbo frames! this makes things very slow!
#define MAX_RCV_PACKETS -1


/* Ethernet header */
struct eth_header {
        u_char  eth_dhost[ETH_ADDR_LEN]; 
        u_char  eth_shost[ETH_ADDR_LEN]; 
        u_short eth_type;              
};

/* IP header */
struct ip_header {
        u_char  ip_vhl;                 /* version << 4 | header length >> 2 */
        u_char  ip_tos;                 /* type of service */
        u_short ip_len;                 /* total length */
        u_short ip_id;                  /* identification */
        u_short ip_off;                 /* fragment offset field */
        #define IP_RF 0x8000            /* reserved fragment flag */
        #define IP_DF 0x4000            /* dont fragment flag */
        #define IP_MF 0x2000            /* more fragments flag */
        #define IP_OFFMASK 0x1fff       /* mask for fragmenting bits */
        u_char  ip_ttl;                 /* time to live */
        u_char  ip_p;                   /* protocol */
        u_short ip_sum;                 /* checksum */
        struct  in_addr ip_src,ip_dst;  /* source and dest address */
};
#define IP_HEADER_LEN(ip)   (((ip)->ip_vhl) & 0x0f)
#define IP_VER(ip)          (((ip)->ip_vhl) >> 4)

struct tcp_header {
        u_short th_sport;               /* source port */
        u_short th_dport;               /* destination port */

        u_int th_seq;                   /* sequence number */
        u_int th_ack;                   /* acknowledgement number */
        u_char  th_offx2;               /* data offset, rsvd */
#define TH_OFF(th)      (((th)->th_offx2 & 0xf0) >> 4)
        u_char  th_flags;
        #define TH_FIN  0x01
        #define TH_SYN  0x02
        #define TH_RST  0x04
        #define TH_PUSH 0x08
        #define TH_ACK  0x10
        #define TH_URG  0x20
        #define TH_ECE  0x40
        #define TH_CWR  0x80
        #define TH_FLAGS        (TH_FIN|TH_SYN|TH_RST|TH_ACK|TH_URG|TH_ECE|TH_CWR)
        u_short th_win;                 /* window */
        u_short th_sum;                 /* checksum */
        u_short th_urp;                 /* urgent pointer */
};


#define FLOW_INIT 0
#define FLOW_SYNACK 1
#define FLOW_HTTP 2
#define FLOW_NOT_HTTP -2
#define FLOW_HTTP_RESP_HEADER_WAIT 3
#define FLOW_HTTP_RESP_HEADER_COMPLETE 4
#define FLOW_HTTP_RESP_MZ_WAIT 5
#define FLOW_HTTP_RESP_MZ_FOUND 6
#define FLOW_HTTP_RESP_MZ_NOT_FOUND -6
#define FLOW_PE_DUMP 7

#define PE_FOUND 1
#define PE_NOT_FOUND -1
#define PE_WAIT_FOR_RESP_BODY 0

#define KB_SIZE 1024
#define MAX_PE_FILE_SIZE 2*KB_SIZE*KB_SIZE
#define MAX_KEY_LEN 60 // larger than really needed
#define MAX_URL_LEN 512
#define MAX_HOST_LEN 256
#define MAX_REFERER_LEN 512
#define MAX_DUMPDIR_LEN 256
#define MAX_NIC_NAME_LEN 10 // larger than really needed
#define TMP_SUFFIX_LEN 4
#define MAX_SC_INIT_PAYLOADS 4
#define INIT_SC_PAYLOAD 1460*4
#define REALLOC_SC_PAYLOAD 100*1024 // 100KB

#define TRUE 1
#define FALSE 0

struct tcp_flow {
        short flow_status;

        // Client->Server half-flow
        char cs_key[MAX_KEY_LEN+1];
        char anon_cs_key[MAX_KEY_LEN+1]; // anonymized cs_key
        char url[MAX_URL_LEN+1];    // URL (including HTTP mothod and HTTP/1.x)
        char host[MAX_HOST_LEN+1];   // Host: header field
        char referer[MAX_REFERER_LEN+1];   // Host: header field

        // Server->Client half-flow
        char sc_key[MAX_KEY_LEN+1];
        u_int sc_init_seq;     // The sequence number of the first packet in the payload buffer 
        u_int sc_expected_seq; // The next expected sequence number 
        u_char* sc_payload;
        u_int sc_payload_size;     // Indicates the current number of bytes in the flow payload 
        u_int sc_payload_capacity; // Indicates the current capacity of the payload buffer 
        u_int sc_num_payloads; // number of packets sent with payload_size > 0
        seq_list_t *sc_seq_list;

        short corrupt_pe; // TRUE or FALSE; records whether the reconstructed PE is believed to be corrupt

        /* Stores the number of the requests in the connection. Useful when there
           is more than one http request for executables in the same 
           connection. This is appended to the name of the dumped file */
        int http_request_count;
};


#define PE_FILE_NAME_LEN 120
struct mz_payload_thread {
        char pe_file_name[PE_FILE_NAME_LEN+1];
        char url[MAX_URL_LEN+1];     // URL (including HTTP mothod and HTTP/1.x)
        char host[MAX_HOST_LEN+1];   // Host: header field
        char referer[MAX_REFERER_LEN+1];   // Referer: header field
        short corrupt_pe; // records if PE is believed to be corrupt
        char *pe_payload;
        u_int pe_payload_size;
        seq_list_t *sc_seq_list;
};


pcap_t *pch;             /* packet capture handler */
struct bpf_program pcf;  /* compiled BPF filter */

struct pcap_stat stats;
struct pcap_stat *statsp;

int anonymize_srcip = TRUE; // used to anonymize client IP for all downloads and debug info
unsigned long xor_key = 0;

int max_pe_file_size;
char *dump_dir;
char *nic_name;
lru_cache_t *lruc;

static void stop_pcap(int);
static void print_stats(int);
void print_usage(char* cmd);
void packet_received(char *args, const struct pcap_pkthdr *header, const u_char *packet);
struct tcp_flow* init_flow(const struct ip_header  *ip, const struct tcp_header *tcp, const char *key, const char *rev_key, const char *anon_key);
struct tcp_flow* lookup_flow(lru_cache_t *lruc, const char *key);
void store_flow(lru_cache_t *lruc, const char *key, struct tcp_flow *tflow);
void remove_flow(lru_cache_t *lruc, struct tcp_flow *tflow);
void update_flow(struct tcp_flow *tflow, const struct tcp_header *tcp, const char *payload, const int payload_size);
void reset_flow_payload(struct tcp_flow *tflow);
void tflow_destroy(void *e);

char *boyermoore_search(const char *haystack, const char *needle);
void get_key(char *key, const char* pkt_src, const char *pkt_dst);
void itoa(int n, char *s);
void reverse(char *s);
int is_http_request(const char *payload, int payload_size);
int is_complete_http_resp_header(const struct tcp_flow *tflow);
int contains_pe_file(const struct tcp_flow *tflow);
char *get_url(const char *payload, int payload_size);
char *get_host(const char *payload, int payload_size);
char *get_referer(const char *payload, int payload_size);
int get_content_length(const char *payload, int payload_size);
int get_resp_hdr_length(const char *payload, int payload_size);
int parse_content_length_str(const char *cl_str);
short is_missing_flow_data(seq_list_t *l, int contentlen);
void dump_pe(struct tcp_flow *tflow);
void *dump_pe_thread(void* d);


// Debug levels 
// #define PE_DEBUG 1
#define QUIET 1
#define VERBOSE 2
#define VERY_VERBOSE 3
#define VERY_VERY_VERBOSE 4
int debug_level = QUIET;


int stats_num_half_open_tcp_flows = 0;
int stats_num_new_tcp_flows = 0;
int stats_num_new_http_flows = 0;
int stats_num_new_pe_flows = 0;


#define NA_DIR 0
#define CS_DIR 1
#define SC_DIR 2
#define LRUC_SIZE 10000

int main(int argc, char **argv) {

    char *pcap_filter;
    char *pcap_file;
    bpf_u_int32 net;
    char err_str[PCAP_ERRBUF_SIZE];

    int lruc_size = LRUC_SIZE;

    pcap_handler callback = (pcap_handler)packet_received;

    int op, opterr;

    if(argc < 3) {
        print_usage(argv[0]);
        exit(1);
    }


    max_pe_file_size = MAX_PE_FILE_SIZE;
    dump_dir = NULL;
    pcap_filter = NULL;
    nic_name = NULL;
    pcap_file = NULL;
    while ((op = getopt(argc, argv, "hi:r:d:f:D:L:K:A")) != -1) {
        switch (op) {

        case 'h':
            print_usage(argv[0]);
            exit(1);
            break;

        case 'A':
            anonymize_srcip = FALSE;
            break;

        case 'i':
            nic_name = strdup(optarg);
            break;

        case 'r':
            pcap_file = optarg;
            break;

        case 'd':
            dump_dir = optarg;
            break;

        case 'f':
            pcap_filter = optarg;
            break;

        case 'D':
            if(atoi(optarg) >= QUIET)
                debug_level = atoi(optarg);
            break;

        case 'L':
            if(atoi(optarg) > 0)
                lruc_size = atoi(optarg);
            break;

        case 'K':
            if(atoi(optarg) > 0)
                max_pe_file_size = atoi(optarg) * KB_SIZE; // size in KB
            break;
        }
    }

    // initialize anonymization key
    if(anonymize_srcip)
        xor_key = (unsigned long)time(NULL);

    printf("Starting %s...\n", argv[0]);
    printf("MAX PE FILE SIZE = %d KB\n", max_pe_file_size/KB_SIZE);
    printf("LRU CACHE SIZE = %d\n",lruc_size);

    signal(SIGTERM, stop_pcap);
    signal(SIGINT,  stop_pcap);
    signal(SIGUSR1, print_stats);

    if(dump_dir == NULL) {
        fprintf(stderr, "dump_dir must to be specified\n");
        print_usage(argv[0]);
        exit(1);
    }

    if(dump_dir != NULL) {
        struct stat stat_buf;
        if(stat(dump_dir, &stat_buf) != 0) {
            fprintf(stderr, "dump_dir %s not found\n", dump_dir);
            print_usage(argv[0]);
            exit(1);
        }
        printf("DUMP DIR = %s\n", dump_dir);
    }


        
    lruc = lruc_init(lruc_size, tflow_destroy);


    // Start reading from device or file
    *err_str = '\0';
    if(pcap_file!=NULL) {
        pch = pcap_open_offline(pcap_file,err_str);
        if(pch == NULL) {
            fprintf(stderr, "Couldn't open the file %s: %s\n",pcap_file,err_str);
            exit(1);
        }
        printf("Reading from %s\n", pcap_file);
    }
    else if(nic_name!=NULL) {
        pch = pcap_open_live(nic_name, PCAP_SNAPLEN, 1, 1000, err_str);
        if (pch == NULL) {
            fprintf(stderr, "Couldn't open device %s: %s", nic_name, err_str);
            exit(1);
        }
        printf("Listening on %s\n", nic_name);
    }

    /* make sure we are capturing from Ethernet device */
    if(pcap_datalink(pch) != DLT_EN10MB) {
        fprintf(stderr, "Device is not an Ethernet\n");
        exit(EXIT_FAILURE);
    }

    /* BPF filter */
    if(pcap_filter == NULL)
        pcap_filter = "tcp"; // default filter
    if(pcap_compile(pch, &pcf, pcap_filter, 0, net) == -1) {
        fprintf(stderr, "Couldn't parse filter %s: %s\n",pcap_filter, pcap_geterr(pch));
        exit(1);
    }

    /* apply BPF filter */
    if (pcap_setfilter(pch, &pcf) == -1) {
        fprintf(stderr, "Couldn't set filter %s: %s\n",pcap_filter, pcap_geterr(pch));
        exit(1);
    }

    printf("BPF FILTER = %s\n", pcap_filter);
    printf("Reading packets...\n\n");

    /* start listening */
    pcap_loop(pch, MAX_RCV_PACKETS, callback, NULL);

    printf("Done reading packets!\n\n");
    sleep(3); // wait just to make sure threats are done...
}

void print_usage(char* cmd) {
    fprintf(stderr, "Usage: %s [-i nic] [-r pcap_file] -d dump_dir [-f \"pcap_filter\"] [-L lru_cache_size] [-K max_pe_file_size (KB)] [-D debug_level] [-A]\n",cmd);
}

void packet_received(char *args, const struct pcap_pkthdr *header, const u_char *packet) {

    if(header->len > PCAP_SNAPLEN) { // skip truncated packets

    #ifdef PE_DEBUG
    if(debug_level >= VERY_VERY_VERBOSE) {
	printf("LEN > PCAP_SNAPLEN !!!");
        printf("header->len = %u\n", header->len);
        printf("PCAP_SNAPLEN = %u\n", PCAP_SNAPLEN);
        fflush(stdout);
    }
    #endif

        return;
    }

    static u_int pkt_count = 0;
    char pkt_src[24], anon_pkt_src[24];
    char pkt_dst[24];

    pkt_count++;

    #ifdef PE_DEBUG
    if(debug_level >= VERY_VERY_VERBOSE) {
        printf("Packet %d\n", pkt_count);
        fflush(stdout);
    }
    #endif

    const struct eth_header *eth;
    const struct ip_header  *ip;
    const struct tcp_header *tcp;
    const char* payload;
    struct tcp_flow *tflow;

    u_int ip_hdr_size;
    u_int tcp_hdr_size;
    int payload_size;

    eth = (const struct eth_header*)packet;
    ip  = (const struct ip_header*)(packet + ETH_HEADER_LEN);
    ip_hdr_size = IP_HEADER_LEN(ip)*4;
    tcp = (const struct tcp_header*)(packet + ETH_HEADER_LEN + ip_hdr_size); 
    tcp_hdr_size = TH_OFF(tcp)*4;
    payload = (const char*)(packet + ETH_HEADER_LEN + ip_hdr_size + tcp_hdr_size);
    payload_size = ntohs(ip->ip_len) - (ip_hdr_size + tcp_hdr_size);

    if(ip_hdr_size < 20 || tcp_hdr_size < 20) {
        #ifdef PE_DEBUG
        if(debug_level >= VERY_VERY_VERBOSE) {
            printf("Invalid packet (headers are tool small)\n");
            fflush(stdout);
        }
        #endif
        return;
    }


    #ifdef PE_DEBUG
    if(debug_level >= VERY_VERY_VERBOSE) {
        // sprintf(pkt_src,"%s:%d",inet_ntoa(ip->ip_src),ntohs(tcp->th_sport));
        sprintf(pkt_dst,"%s:%d",inet_ntoa(ip->ip_dst),ntohs(tcp->th_dport));
        // printf("Src = %s\n",pkt_src);
        printf("Dst = %s\n",pkt_dst);
        printf("ip_len = %u\n", ntohs(ip->ip_len));
        printf("ip_hdr_size = %u\n", ip_hdr_size);
        printf("tcp_hdr_size = %u\n", tcp_hdr_size);
        printf("payload_size = %u\n", payload_size);
        // printf("%s", payload);
        fflush(stdout);
    }
    #endif


    // Skip ACK-only packets or other empty packets
    if(payload_size == 0 && !((tcp->th_flags & TH_SYN) || (tcp->th_flags & TH_FIN) || (tcp->th_flags & TH_RST))) {
        #ifdef PE_DEBUG
        if(debug_level >= VERY_VERY_VERBOSE) {
            printf("skeeping empty packet \n");
            fflush(stdout);
        }
        #endif
        return;
    }

    char key[MAX_KEY_LEN];
    char anon_key[MAX_KEY_LEN];
    char rev_key[MAX_KEY_LEN];

    sprintf(pkt_src,"%s:%d",inet_ntoa(ip->ip_src),ntohs(tcp->th_sport));
    sprintf(pkt_dst,"%s:%d",inet_ntoa(ip->ip_dst),ntohs(tcp->th_dport));
    get_key(key,pkt_src,pkt_dst);
    get_key(rev_key,pkt_dst,pkt_src);

    anon_key[0] = '\0'; // empty
    if(anonymize_srcip) {
        struct in_addr anon_ip_src = ip->ip_src;
        anon_ip_src.s_addr = (anon_ip_src.s_addr ^ xor_key) & 0xFFFFFF00 | 0x0000000A; // --> 10.x.x.x
        sprintf(anon_pkt_src,"%s:%d",inet_ntoa(anon_ip_src),ntohs(tcp->th_sport));
        get_key(anon_key,anon_pkt_src,pkt_dst);
    }

    // Check if this is a new flow
    if(tcp->th_flags == TH_SYN) {
        
        tflow = init_flow(ip, tcp, key, rev_key, anon_key);
        if(tflow == NULL)
            return;

        // in case of 4-tuple collision
        struct tcp_flow *tmp_tflow; 
        if((tmp_tflow = lookup_flow(lruc, tflow->cs_key)) != NULL) {
            if(tmp_tflow->flow_status == FLOW_HTTP_RESP_MZ_FOUND) {
                // record premature end of flow. PE most likely corrupt
                tmp_tflow->corrupt_pe = TRUE;

                // dump reconstructed PE file
                dump_pe(tmp_tflow);
            }
            remove_flow(lruc, tmp_tflow);
        }
        
        store_flow(lruc, tflow->cs_key, tflow); 
        #ifdef PE_DEBUG
        if(debug_level >= VERY_VERBOSE) {
            printf("Found a new TCP flow: %s\n",tflow->anon_cs_key);
            fflush(stdout);
        }
        #endif

        stats_num_half_open_tcp_flows++;
        return;
    }
    

    short flow_direction = NA_DIR;

    // flow reverse key (server-to-client 4-tuple identifier)
    get_key(rev_key,pkt_dst,pkt_src);

    // we check for SC_DIR first, since there are usually many more SC packets than CS packets
    if((tflow = lookup_flow(lruc, rev_key)) != NULL) {
        #ifdef PE_DEBUG
        if(debug_level >= VERY_VERY_VERBOSE) {
            printf("TCP flow %s, server-to-client direction\n", tflow->anon_cs_key);
            fflush(stdout);
        }
        #endif

        flow_direction = SC_DIR;
    }
    else {
        get_key(rev_key,pkt_src,pkt_dst);

        if((tflow = lookup_flow(lruc, key)) != NULL) {
            #ifdef PE_DEBUG
            if(debug_level >= VERY_VERY_VERBOSE) {
                printf("TCP flow %s, client-to-server direction\n", tflow->anon_cs_key);
                fflush(stdout);
            }
            #endif

            flow_direction = CS_DIR;
        }
        else
            return;
    }
    

    // check if it appears the flow is being closed
    if((tcp->th_flags & TH_RST) || (tcp->th_flags & TH_FIN)) {
        #ifdef PE_DEBUG
        if(debug_level >= VERY_VERBOSE) {
            printf("TCP flow %s is being closed\n", tflow->anon_cs_key);
            fflush(stdout);
        }
        #endif

        // if this flow contains a PE file, dump it
        if(tflow->flow_status == FLOW_HTTP_RESP_MZ_FOUND) {
            #ifdef PE_DEBUG
            if(debug_level >= VERY_VERBOSE) {
                printf("PE flow %s is being closed and dumped: payload size = %d\n", tflow->anon_cs_key, tflow->sc_payload_size);
                printf("Flow Direction = %d\n", flow_direction);
                fflush(stdout);
            }
            #endif

            if(flow_direction == SC_DIR) {
                update_flow(tflow, tcp, payload, payload_size);
                // record what was the last PE byte in the S->C half flow (from server's FIN)
                seq_list_insert(tflow->sc_seq_list, ntohl(tcp->th_seq), payload_size);
            }
            else {
                // record what was the last expected PE byte from the server (from in the client's FIN)
                seq_list_insert(tflow->sc_seq_list, ntohl(tcp->th_ack), 0);
            }

            // dump the reconstructed PE file
            dump_pe(tflow);

            // start looking for another PE file in the same HTTP connection
            tflow->flow_status = FLOW_HTTP;
        }
            
        // remove this flow from the cache of open tcp flows
        #ifdef PE_DEBUG
        if(debug_level >= VERY_VERBOSE) {
            printf("TCP flow is being removed from the cache\n");
            fflush(stdout);
        }
        #endif

        remove_flow(lruc, tflow);
        return;
    }


    if(flow_direction == CS_DIR) {
    // Clinet to Server packet. Check and update HTTP query status


        // if first request packet
        if(tflow->flow_status == FLOW_SYNACK) {
            if(!is_http_request(payload, payload_size)) {
                #ifdef PE_DEBUG
                if(debug_level >= VERY_VERBOSE) {
                    printf("Non-HTTP TCP flow is being removed from the cache\n");
                    fflush(stdout);
                }       
                #endif

                remove_flow(lruc, tflow);
                return;
            }

            tflow->flow_status = FLOW_HTTP;
            stats_num_new_http_flows++;
        }

        if(tflow->flow_status == FLOW_HTTP && !is_http_request(payload, payload_size)) {
            // something strage happend... we'll wait for next valid HTTP request
            return;
        }
        
        if(tflow->flow_status == FLOW_HTTP_RESP_MZ_FOUND) {
            #ifdef PE_DEBUG
            if(debug_level >= VERBOSE) {
                printf("PE flow %s is being closed and dumped (new HTTP req): payload size = %d\n", tflow->anon_cs_key, tflow->sc_payload_size);
                fflush(stdout);
            }
            #endif

            // record the last PE byte expected from the server (from client's ack)
            seq_list_insert(tflow->sc_seq_list, ntohl(tcp->th_ack), 0);

            // dump reconstructed PE file
            dump_pe(tflow);

            // wait for a new HTTP request
            tflow->flow_status = FLOW_HTTP;
        }
    
        // We only consider the very first packet of each HTTP req to extract URL and Host
        // Currently we cannot deal with packet reordering for HTTP req in multiple packets
        if(is_http_request(payload, payload_size) && tflow->flow_status != FLOW_HTTP_RESP_HEADER_WAIT) { 

            tflow->flow_status = FLOW_HTTP_RESP_HEADER_WAIT;
            tflow->http_request_count++;

            strncpy(tflow->url, get_url(payload, payload_size), MAX_URL_LEN);
            tflow->url[MAX_URL_LEN] = '\0'; // just to be extra safe...
            strncpy(tflow->host, get_host(payload, payload_size), MAX_HOST_LEN);
            tflow->host[MAX_HOST_LEN] = '\0'; // just to be extra safe...
            strncpy(tflow->referer, get_referer(payload, payload_size), MAX_REFERER_LEN);
            tflow->host[MAX_REFERER_LEN] = '\0'; // just to be extra safe...

            #ifdef PE_DEBUG
            if(debug_level >= VERBOSE) {
                printf("Found HTTP request: %s : %s : %s\n",
                    // get_host(tflow->cs_payload, tflow->cs_payload_size), 
                    // get_url(tflow->cs_payload, tflow->cs_payload_size));
                    get_host(payload, payload_size), 
                    get_url(payload, payload_size),
                    get_referer(payload, payload_size));
                printf("Flow status = %d\n", tflow->flow_status);
                fflush(stdout);
            }
            #endif
        } 

    }
    else if(flow_direction == SC_DIR) {
    // Server to Client packet. Check and update HTTP response status

        if((tcp->th_flags & TH_SYN) && (tcp->th_flags & TH_ACK)) {
            #ifdef PE_DEBUG
            if(debug_level >= VERY_VERY_VERBOSE) {
                printf("Found a SYNACK for TCP flow: %s\n",tflow->anon_cs_key);
                fflush(stdout);
            }
            #endif

            stats_num_new_tcp_flows++;

            tflow->flow_status = FLOW_SYNACK;
            return;
        }


        if(tflow->flow_status == FLOW_HTTP) {
            // we are still waiting for a proper HTTP request
            // don't consider resp packets until that happens
            return;
        }

        if(tflow->flow_status == FLOW_HTTP_RESP_MZ_FOUND && tflow->sc_payload_size > max_pe_file_size) {
            // This PE file is too large, skip it!
            tflow->flow_status = FLOW_HTTP;
            reset_flow_payload(tflow);
            return;
        }

        // update tcp seq numbers and payload content
        update_flow(tflow, tcp, payload, payload_size);
        #ifdef PE_DEBUG
        if(debug_level >= VERY_VERY_VERBOSE) {
            printf("Flow %s has been updated \n",tflow->anon_cs_key);
            fflush(stdout);
        }
        #endif

        if(tflow->flow_status == FLOW_HTTP_RESP_HEADER_WAIT) {
            // check for end of HTTP resp

            #ifdef PE_DEBUG
            if(debug_level >= VERY_VERY_VERBOSE) {
                printf("Flow status is FLOW_HTTP_RESP_HEADER_WAIT \n");
                fflush(stdout);
            }
            #endif


            if(is_complete_http_resp_header(tflow)) {
                tflow->flow_status = FLOW_HTTP_RESP_MZ_WAIT;

                #ifdef PE_DEBUG
                if(debug_level >= VERY_VERY_VERBOSE) {
                    printf("Flow status is FLOW_HTTP_RESP_MZ_WAIT \n");
                    fflush(stdout);
                }
                #endif
            }
            else if(tflow->sc_num_payloads > MAX_SC_INIT_PAYLOADS) {
                #ifdef PE_DEBUG
                if(debug_level >= VERY_VERY_VERBOSE) {
                    printf("tflow->sc_num_payloads > MAX_SC_INIT_PAYLOADS \n");
                    fflush(stdout);
                }
                #endif

                tflow->flow_status = FLOW_HTTP; // return to wait for new HTTP request
                reset_flow_payload(tflow);

                #ifdef PE_DEBUG
                if(debug_level >= VERY_VERY_VERBOSE) {
                    printf("Flow status reset to FLOW_HTTP \n");
                    fflush(stdout);
                }
                #endif


                return;
            }
        }

        if(tflow->flow_status == FLOW_HTTP_RESP_MZ_WAIT) {
            #ifdef PE_DEBUG
            if(debug_level >= VERY_VERY_VERBOSE) {
                printf("Flow status is FLOW_HTTP_RESP_MZ_WAIT \n");
                fflush(stdout);
            }
            #endif

            int resp = PE_NOT_FOUND;
            int contentlen = get_content_length(tflow->sc_payload, tflow->sc_payload_size);

            if (contentlen > 0 && contentlen < MAX_PE_FILE_SIZE) // otherwise we don't even try to check if there is a large PE file... force to abandon this flow!
                resp = contains_pe_file(tflow);

            if(resp == PE_FOUND) {
                tflow->flow_status = FLOW_HTTP_RESP_MZ_FOUND;
                stats_num_new_pe_flows++;
                // #ifdef PE_DEBUG
                // if(debug_level >= QUIET) {
                    printf("Found PE flow : %s\n", tflow->anon_cs_key);
                    fflush(stdout);
                // }
                // #endif
            }
            else if(resp == PE_NOT_FOUND) {
                tflow->flow_status = FLOW_HTTP; // return to wait for new HTTP request
                reset_flow_payload(tflow);
            }
            else if(resp == PE_WAIT_FOR_RESP_BODY) {
                if(tflow->sc_num_payloads > MAX_SC_INIT_PAYLOADS) {
                    #ifdef PE_DEBUG
                    if(debug_level >= VERY_VERY_VERBOSE) {
                        printf("tflow->sc_num_payloads > MAX_SC_INIT_PAYLOADS \n");
                        fflush(stdout);
                    }
                    #endif
                    
                    // we are not going to wait any longer
                    tflow->flow_status = FLOW_HTTP;
                    reset_flow_payload(tflow);
                }
            }
        }

    }
}


// Called at new TCP SYN
struct tcp_flow* init_flow(const struct ip_header  *ip, const struct tcp_header *tcp, const char *key, const char *rev_key, const char *anon_key) {

    struct tcp_flow *tflow = (struct tcp_flow*)malloc(sizeof(struct tcp_flow));
    if(tflow == NULL)
        return NULL;

    tflow->flow_status = FLOW_INIT;
    
    strcpy(tflow->cs_key, key);
    if(strlen(anon_key)>0)
        strcpy(tflow->anon_cs_key, anon_key);
    else
        strcpy(tflow->anon_cs_key, key);
    strcpy(tflow->sc_key, rev_key);

    tflow->url[0] = '\0';
    tflow->host[0] = '\0';
    tflow->sc_payload = NULL;    

    tflow->sc_init_seq = 0;
    tflow->sc_expected_seq = 0;
    tflow->sc_payload_size = 0;
    tflow->sc_payload_capacity = 0;
    tflow->sc_num_payloads = 0;
    tflow->sc_seq_list = NULL;

    tflow->corrupt_pe = FALSE;
    tflow->http_request_count = 0;

    return tflow;
}


struct tcp_flow* lookup_flow(lru_cache_t *lruc, const char *key) {
    return (struct tcp_flow*) lruc_search(lruc, key);
}


void store_flow(lru_cache_t *lruc, const char *key, struct tcp_flow *tflow) {
    lruc_insert(lruc, tflow->cs_key, tflow);
} 


void remove_flow(lru_cache_t *lruc, struct tcp_flow *tflow) {
    lruc_delete(lruc, tflow->cs_key);
}


void tflow_destroy(void *v) {

    struct tcp_flow *tflow = (struct tcp_flow *)v;

    if(tflow == NULL)
        return;

    #ifdef PE_DEBUG
    if(debug_level >= VERY_VERY_VERBOSE) {
        printf("Destroying LRUC entry for %s\n", tflow->anon_cs_key);
        fflush(stdout);
    }
    #endif

    if(tflow->sc_payload != NULL) {
        if(tflow->flow_status == FLOW_HTTP_RESP_MZ_FOUND) {
            // record the fact that flow is being unexpectedly terminated, PE most likely corrupt
            tflow->corrupt_pe = TRUE;

            // dump reconstructed PE file
            dump_pe(tflow);
        }

        free(tflow->sc_payload);
        tflow->sc_payload = NULL;

        #ifdef PE_DEBUG
        if(debug_level >= VERY_VERY_VERBOSE) {
            printf("Destroyed tflow->sc_payload\n");
            fflush(stdout);
        }
        #endif
    }

    if(tflow->sc_seq_list != NULL) {
        seq_list_destroy(tflow->sc_seq_list);
        free(tflow->sc_seq_list);
        tflow->sc_seq_list = NULL;

        #ifdef PE_DEBUG
        if(debug_level >= VERY_VERY_VERBOSE) {
            printf("Destroyed tflow->sc_seq_list\n");
            fflush(stdout);
        }
        #endif
    }
    
    free(tflow);

    #ifdef PE_DEBUG
    if(debug_level >= VERY_VERY_VERBOSE) {
        printf("Destroyed LRUC entry \n");
        fflush(stdout);
    }
    #endif
}


static void stop_pcap(int signo) {
    print_stats(signo);
    exit(1);
}


static void print_stats(int signo) {
    struct pcap_stat stat;

    fprintf(stderr, "----------------------------------\n");
    if (pcap_stats(pch, &stat) < 0) {
        fprintf(stderr, "pcap_stats: %s\n", pcap_geterr(pch));
        return;
    }
    // fprintf(stderr, "%u packets captured \n", packets_captured);
    fprintf(stderr, "%d packets received by filter \n", stat.ps_recv);
    fprintf(stderr, "%d packets dropped by kernel\n", stat.ps_drop);
    fprintf(stderr, "%d number of new half-open (SYN) tcp flows\n", stats_num_half_open_tcp_flows);
    fprintf(stderr, "%d number of new (SYN ACK) tcp flows\n", stats_num_new_tcp_flows);
    fprintf(stderr, "%d number of new http flows\n", stats_num_new_http_flows);
    fprintf(stderr, "%d number of new PE flows\n", stats_num_new_pe_flows);

    fprintf(stderr, "----------------------------------\n");
}


// we are only interested in GET, POST, and HEAD requests
int is_http_request(const char *payload, int payload_size) {

    if(payload_size < 5)
        return 0;

    if(strncmp("GET ", payload, 4) == 0)
        return 1;

    if(strncmp("POST ", payload, 5) == 0)
        return 1;

    if(strncmp("HEAD ", payload, 5) == 0)
        return 1;

    return 0;

}


char *get_url(const char *payload, int payload_size) {

    int i;
    static char url[MAX_URL_LEN+1];

    for(i=0; i<MAX_URL_LEN && i<payload_size; i++) {
        if(payload[i] == '\r' || payload[i] == '\n') {
            // the first condition should suffice, but we check both just for sure...
            break;
        }

        url[i]=payload[i];
    }
    url[i]='\0'; // make sure it's properly terminated

    // printf("URL: %s\n",url);
    // fflush(stdout);
    
    return url;
}


char *get_host(const char *payload, int payload_size) {

    static char host[MAX_HOST_LEN+1];
    char haystack[payload_size+1];
    const char needle[] = "\r\nHost:";
    char *p = NULL;
    int i;

    strncpy(haystack, payload, payload_size);
    haystack[payload_size]='\0'; // just to be safe...

    if(strlen(haystack) < strlen(needle))
        return "\0";

    p = boyermoore_search(haystack,needle);
    
    if(p == NULL) 
        host[0] = '\0';
    else {
        p += 2; // skip \r\n
        for(i=0; i<MAX_HOST_LEN && (p+i) < &(haystack[payload_size]); i++) {
            if(*(p+i) == '\r' || *(p+i) == '\n')
                break;
            host[i]=*(p+i);
        }
        host[i]='\0';
    }

    return host;
}


char *get_referer(const char *payload, int payload_size) {

    static char referer[MAX_REFERER_LEN+1];
    char haystack[payload_size+1];
    const char needle[] = "\r\nReferer:";
    char *p = NULL;
    int i;

    strncpy(haystack, payload, payload_size);
    haystack[payload_size]='\0'; // just to be safe...

    if(strlen(haystack) < strlen(needle))
        return "\0";

    p = boyermoore_search(haystack,needle);
    
    if(p == NULL) 
        referer[0] = '\0';
    else {
        p += 2; // skip \r\n
        for(i=0; i<MAX_REFERER_LEN && (p+i) < &(haystack[payload_size]); i++) {
            if(*(p+i) == '\r' || *(p+i) == '\n')
                break;
            referer[i]=*(p+i);
        }
        referer[i]='\0';
    }

    return referer;
}


int is_complete_http_resp_header(const struct tcp_flow *tflow) {
    if(tflow->sc_payload == NULL)
        return 0;

    #ifdef PE_DEBUG
    if(debug_level >= VERY_VERY_VERBOSE) {
        printf("Checking if complete HTTP header\n");
        fflush(stdout);
        printf("lenght of tflow->sc_payload = %d, tflow->sc_payload_size = %d \n", strlen(tflow->sc_payload),tflow->sc_payload_size);
        fflush(stdout);
    }
    #endif

    const char needle[] = "\r\n\r\n";
    int needle_len = 4;

    if(strnlen(tflow->sc_payload,needle_len) < needle_len)
        return 0;

    char *p = boyermoore_search(tflow->sc_payload,needle);

    if(p != NULL)
        return 1;

    return 0;
}



int get_resp_hdr_length(const char *payload, int payload_size) {

    #define HDR_SEARCH_LIMIT 3*1024 // we expect to find "\r\n\r\n" withing first 3kB

    int search_limit = MIN(HDR_SEARCH_LIMIT,payload_size);
    char haystack[search_limit+1];
    const char needle[] = "\r\n\r\n";
    char *p = NULL;
    int i;

    strncpy(haystack, payload, search_limit);
    haystack[search_limit]='\0'; // just to be safe...

    if(strlen(haystack) < strlen(needle))
        return -1;

    p = boyermoore_search(haystack,needle);
    
    if(p == NULL) 
        return -1;
    else 
        return p-haystack+strlen(needle);
}



int get_content_length(const char *payload, int payload_size) {

    #define MAX_CONTENTLENGTH_LEN 40
    #define CL_SEARCH_LIMIT 3*1024 // we expect to find the content lenght withing first 3kB

    int cl_search_limit = MIN(CL_SEARCH_LIMIT,payload_size);
    char contentlen_str[MAX_CONTENTLENGTH_LEN+1];
    char haystack[cl_search_limit+1];
    const char needle[] = "\r\nContent-Length:";
    char *p = NULL;
    int i;

    strncpy(haystack, payload, cl_search_limit);
    haystack[cl_search_limit]='\0'; // just to be safe...

    if(strlen(haystack) < strlen(needle))
        return -1;

    p = boyermoore_search(haystack,needle);
    
    if(p == NULL) 
        return -1;
    else {
        p += 2; // skip \r\n

        for(i=0; i<MAX_CONTENTLENGTH_LEN && (p+i) < &(haystack[cl_search_limit]); i++) {
            if(*(p+i) == '\r' || *(p+i) == '\n')
                break;
            contentlen_str[i]=*(p+i);
        }
        contentlen_str[i]='\0';
    }


    // parse contentlen_str and return an integer
    int k = strlen(contentlen_str);
    p = contentlen_str;

    while((*p)!=':' && p<p+k)
        p++;
    p++; // skip ':'

    return atoi(p);

}


int parse_content_length_str(const char *cl_str) {

   int k = strlen(cl_str);
   char *p = (char*)cl_str;

   while((*p)!=':' && p<p+k) 
       p++;

   return atoi(p);

}


int contains_pe_file(const struct tcp_flow *tflow) {
    if(tflow->sc_payload == NULL)
        return PE_WAIT_FOR_RESP_BODY;

    #define MIN_PE_PAYLOAD_SIZE 14
    if(tflow->sc_payload_size < MIN_PE_PAYLOAD_SIZE)
	return PE_WAIT_FOR_RESP_BODY;

    if(strncmp(tflow->sc_payload,"HTTP/",5)!=0) // payload must begin with "HTTP/", otherwise it's not a valid HTTP response
        return PE_NOT_FOUND;

    #define HTTP_200_OFFSET 8 // strlen("HTTP/x.x") == 8
    if(strncmp(tflow->sc_payload+HTTP_200_OFFSET," 200 ",5)!=0) // we only accept 200 OK responses (check for "200" right after "HTTP/x.x")
        return PE_NOT_FOUND;

    const char needle[] = "\r\n\r\n";
    int needle_len = 4;

    if(strnlen(tflow->sc_payload,needle_len) < needle_len)
        return PE_WAIT_FOR_RESP_BODY; // this should never happen!

    char *p = boyermoore_search(tflow->sc_payload,needle);

    if(p == NULL) // this should not happen becaus the resp header should be already complete
        return PE_NOT_FOUND;

    if(strnlen(p,needle_len+2) >= needle_len+2) {
        if(p[4] == 'M' && p[5] == 'Z')
            return PE_FOUND;
        else
            return PE_NOT_FOUND;
    }
    
    return PE_WAIT_FOR_RESP_BODY;
}


// looks for gaps in the list of TCP sequence numbers
// returns TRUE is gaps are found
short is_missing_flow_data(seq_list_t *l, int contentlen) {
    // detect gaps in the sequence numbers

    seq_list_entry_t *e;
    u_int seq_num, psize, m;
    u_int expected_seq_num, tmp_expected_seq_num, max_seq_num;
    short gap_detected, terminate_loop;
    int estimated_contentlen;


    // check if this is a non-empy seq_list
    if(l == NULL)
        return TRUE;
    if(seq_list_head(l) == NULL)
        return TRUE;

    // get the max sequence number in the list
    seq_list_restart_from_head(l); // makes sure we start from the head of the list
    e = seq_list_next(l);
    max_seq_num = 0;
    while(e != NULL) {
        seq_num = e->i;
        psize = e->j;
        m = seq_num+psize;
        if(m > max_seq_num)
            max_seq_num = m;

        e = seq_list_next(l);
    }


    // get the first sequence number in the list
    seq_list_restart_from_head(l); // makes sure we start from the head of the list
    e = seq_list_next(l);
    seq_num = e->i;
    psize = e->j;

    estimated_contentlen = max_seq_num - seq_num;
    if(estimated_contentlen < contentlen)
        return TRUE;


    #ifdef PE_DEBUG
    if(debug_level >= VERY_VERY_VERBOSE) {
        printf("max_seq_num = %u  ", max_seq_num);
        printf("estimated content length = %u \n", estimated_contentlen);
        seq_list_print(l);
        fflush(stdout);
    }
    #endif

    expected_seq_num = seq_num + psize; // initialize first expected_seq_num
    tmp_expected_seq_num = expected_seq_num;

    terminate_loop = FALSE;
    do {
        gap_detected = FALSE; 

        while((e = seq_list_next(l)) != NULL) {
            seq_num = e->i;
            psize = e->j;

            if(seq_num == 0 && psize == 0) // ignore pairs marked to zero
                continue;

            #ifdef PE_DEBUG
            if(debug_level >= VERY_VERY_VERBOSE) {
                printf("seq_num = %u , psize = %u \n", seq_num, psize);
                fflush(stdout);
            }
            #endif

            // ignore retransmissions
            if(seq_num <= tmp_expected_seq_num && (seq_num+psize) <= tmp_expected_seq_num)
                continue;
        
            // account for reordering or retransmissions with overlapping sequence numbers
            if(seq_num <= tmp_expected_seq_num && (seq_num+psize) >= tmp_expected_seq_num) {
                tmp_expected_seq_num = seq_num + psize;

                #ifdef PE_DEBUG
                if(debug_level >= VERY_VERY_VERBOSE) {
                    printf("tmp_expected_seq_num = %u \n", tmp_expected_seq_num);
                    fflush(stdout);
                }
                #endif

                // mark to zero, so that this pair of (seq_num,psize) will not be considered anymore
                e->i = 0;
                e->j = 0;
            }
            else {
                #ifdef PE_DEBUG
                if(debug_level >= VERY_VERY_VERBOSE) {
                    printf("====> FOUND GAP: ");
                    printf("seq_num = %u  ", seq_num);
                    printf("psize = %u  ", psize);
                    printf("tmp_expected_seq_num = %u \n", tmp_expected_seq_num);
                    fflush(stdout);
                }
                #endif
                gap_detected = TRUE;
            }

        }

        if(tmp_expected_seq_num == expected_seq_num) 
        // we either reached the end, or noting has improved in this iteration
            terminate_loop = TRUE; // do not "break", because we want to execute the following code before exiting the while


        // prepare for a new iteration
        expected_seq_num = tmp_expected_seq_num;
        seq_list_restart_from_head(l); // makes sure we start from the head of the list

        #ifdef PE_DEBUG
        if(debug_level >= VERY_VERY_VERBOSE) {
            printf("max_seq_num = %u  ", max_seq_num);
            printf("expected_seq_num = %u  ", expected_seq_num);
            printf("tmp_expected_seq_num = %u  ", tmp_expected_seq_num);
            printf("GAP = %d  \n", gap_detected);
            fflush(stdout);
        }
        #endif

    }
    while(gap_detected && !terminate_loop);


    if(gap_detected) { // detected missing data
        #ifdef PE_DEBUG
        if(debug_level >= VERY_VERY_VERBOSE) 
	        printf("DETECTED MISSING DATA\n");
        #endif

        return TRUE;
    }


    #ifdef PE_DEBUG
    if(debug_level >= VERY_VERY_VERBOSE)
        printf("NO MISSING DATA\n");
    #endif

    return FALSE; // everything looks fine!
}


void dump_pe(struct tcp_flow *tflow) {
// make a copy of all buffers
// free mamory that is not needed

    if(tflow->sc_payload == NULL)
        return;

    #ifdef PE_DEBUG
    if(debug_level >= VERY_VERY_VERBOSE) {
        printf("Preparing data for dumping: %s\n",tflow->anon_cs_key);
        fflush(stdout);
    }
    #endif

    struct mz_payload_thread *tdata;
    pthread_t thread_id;

    tdata = (struct mz_payload_thread*)malloc(sizeof(struct mz_payload_thread));
    if(tdata == NULL)
        return;

    sprintf(tdata->pe_file_name,"%s-%d",tflow->anon_cs_key,tflow->http_request_count);
    strcpy(tdata->url,tflow->url);
    strcpy(tdata->host,tflow->host);
    strcpy(tdata->referer,tflow->referer);
    tdata->pe_payload = tflow->sc_payload;
    tdata->pe_payload_size = tflow->sc_payload_size;
    tdata->corrupt_pe = tflow->corrupt_pe;
    tdata->sc_seq_list = tflow->sc_seq_list;
    tflow->sc_seq_list = NULL; // prevents a possible double free

    #ifdef PE_DEBUG
    if(debug_level >= VERY_VERY_VERBOSE) {
        printf("Creating dumping thread: %s\n",tflow->anon_cs_key);
        fflush(stdout);
    }
    #endif
    
    tflow->sc_payload = NULL; // avoids buffer to be freed by main thread
    pthread_create(&thread_id,NULL,dump_pe_thread,(void*)tdata);

}

void *dump_pe_thread(void* d) {

    #define FNAME_LEN MAX_DUMPDIR_LEN+MAX_NIC_NAME_LEN+MAX_KEY_LEN+3
    char fname[FNAME_LEN];
    char tmp_fname[FNAME_LEN+TMP_SUFFIX_LEN+1];
    FILE* pe_file;
    struct mz_payload_thread *tdata = (struct mz_payload_thread*) d;

    if(tdata == NULL || tdata->pe_payload == NULL)
        return;

    #ifdef PE_DEBUG
    if(debug_level >= VERY_VERY_VERBOSE) {
        printf("Preparing dump file...\n");
        fflush(stdout);
    }
    #endif

    fname[0]='\0';
    strncat(fname,dump_dir,MAX_DUMPDIR_LEN);
    strncat(fname,"/",1);
    if(nic_name != NULL) {
        strncat(fname,nic_name,MAX_NIC_NAME_LEN);
        strncat(fname,"~",1);
    }
    strncat(fname,tdata->pe_file_name,MAX_KEY_LEN);


    strncpy(tmp_fname,fname,FNAME_LEN);
    strncat(tmp_fname,".tmp",TMP_SUFFIX_LEN);
    if((pe_file = fopen(tmp_fname,"wb")) == NULL) {
        fprintf(stderr,"Cannot write to file %s\n",tmp_fname);
	perror("--> ");
        fflush(stderr);
    }
    printf("Writing to file %s\n",tmp_fname);
    fflush(stdout);

    #ifdef PE_DEBUG    
    if(debug_level >= VERY_VERY_VERBOSE) {
        printf("Dumping to file: %s\n",tdata->pe_file_name);
        fflush(stdout);
    }
    #endif

    #define TS_STR_LEN 20
    char ts_str[TS_STR_LEN+1];
    itoa(time(NULL),ts_str);

    fwrite("% ", sizeof(char), 2, pe_file);
    fwrite(ts_str, sizeof(char), strlen(ts_str), pe_file);
    fwrite("\n", sizeof(char), 1, pe_file);
    fwrite("% ", sizeof(char), 2, pe_file);
    fwrite(tdata->pe_file_name, sizeof(char), strlen(tdata->pe_file_name), pe_file);
    fwrite("\n", sizeof(char), 1, pe_file);
    fwrite("% ", sizeof(char), 2, pe_file);
    fwrite(tdata->url, sizeof(char), strlen(tdata->url), pe_file);
    fwrite("\n", sizeof(char), 1, pe_file);
    fwrite("% ", sizeof(char), 2, pe_file);
    fwrite(tdata->host, sizeof(char), strlen(tdata->host), pe_file);
    fwrite("\n", sizeof(char), 1, pe_file);
    fwrite("% ", sizeof(char), 2, pe_file);
    fwrite(tdata->referer, sizeof(char), strlen(tdata->referer), pe_file);
    fwrite("\n", sizeof(char), 1, pe_file);

    int contentlen = get_content_length(tdata->pe_payload, tdata->pe_payload_size);
    int httphdrlen = get_resp_hdr_length(tdata->pe_payload, tdata->pe_payload_size);

    #ifdef PE_DEBUG    
    if(debug_level >= VERY_VERY_VERBOSE) {
        printf("HTTP Response Header Length: %d\n", httphdrlen);
        printf("HTTP Response Body Length: %d\n", contentlen);
    }
    #endif

    if(contentlen <= 0 || httphdrlen <= 0)
        tdata->corrupt_pe = TRUE; 

    if((contentlen+httphdrlen) > tdata->pe_payload_size)
	    tdata->corrupt_pe = TRUE;

    #ifdef PE_DEBUG    
    if(debug_level >= VERY_VERY_VERBOSE) {
        if(tdata->corrupt_pe)
    	    printf("IS CORRUPT\n");
        else
    	    printf("NOT CORRUPT\n");
    }

    printf("\n===\n");
    #endif

    #define CORRUPT_PE_ALERT "CORRUPT_PE"
    short missing_data = is_missing_flow_data(tdata->sc_seq_list, contentlen);


    fwrite("% ", sizeof(char), 2, pe_file);
    if(tdata->corrupt_pe || missing_data) {
        fwrite(CORRUPT_PE_ALERT, sizeof(char), strlen(CORRUPT_PE_ALERT), pe_file);

        #ifdef PE_DEBUG
        if(debug_level >= VERY_VERY_VERBOSE) {
            printf("THIS IS A CORRUPTED BINARY!\n");
            printf("tdata->corrupt_pe = %d , is_missing_flow_data = %d\n", tdata->corrupt_pe, missing_data);
            fflush(stdout);
        }
        #endif
    }
    fwrite("\n", sizeof(char), 1, pe_file);

    fwrite("\n", sizeof(char), 1, pe_file);
    fwrite(tdata->pe_payload, sizeof(char), tdata->pe_payload_size, pe_file);
    fclose(pe_file);

    // rename temporary dump file
    int ren = rename(tmp_fname, fname);
    if(ren < 0) {
        fprintf(stderr,"Unable to rename %s\n",tmp_fname);
        perror("--> ");
        fflush(stderr);
    }
    printf("Renamed dump file to %s\n", fname);
    fflush(stdout);
    

    #ifdef PE_DEBUG    
    if(debug_level >= VERY_VERY_VERBOSE) {
        printf("Closed dumped file: %s\n",tdata->pe_file_name);
        fflush(stdout);
    }
    #endif

    // free the memory
    free(tdata->pe_payload);
    seq_list_destroy(tdata->sc_seq_list);
    free(tdata->sc_seq_list);
    free(tdata);

    #ifdef PE_DEBUG    
    if(debug_level >= VERY_VERY_VERBOSE) {
        printf("Closing thread after freeing tdata: %s\n",tdata->pe_file_name);
        fflush(stdout);
    }
    #endif

    pthread_exit(NULL);
}

 
void update_flow(struct tcp_flow *tflow, const struct tcp_header *tcp, const char *payload, const int payload_size) {

    if(tflow == NULL) // checking just for sure...
        return;

    if(tflow->sc_payload == NULL) {
        #ifdef PE_DEBUG
        if(debug_level >= VERY_VERY_VERBOSE) {
            printf("Flow %s payload is being initialized \n",tflow->anon_cs_key);
            fflush(stdout);
        }
        #endif

        tflow->sc_init_seq = ntohl(tcp->th_seq);
        tflow->sc_payload = (char*)malloc((INIT_SC_PAYLOAD+1)*sizeof(char));
        if(tflow->sc_payload == NULL)
            return;

        memset(tflow->sc_payload, 0, INIT_SC_PAYLOAD+1);
        tflow->sc_payload_size = 0;
        tflow->sc_payload_capacity = INIT_SC_PAYLOAD;
        tflow->sc_num_payloads = 0;
        tflow->sc_seq_list = seq_list_init();
        tflow->corrupt_pe = FALSE;
    }

    if(payload_size == 0)
        return;

    tflow->sc_num_payloads++; //counts number of payload (for now including duplicate, we'll change it later)

    #ifdef PE_DEBUG
    if(debug_level >= VERY_VERY_VERBOSE) {
        printf("Flow %s is being updated. Flow status = %d \n",tflow->anon_cs_key, tflow->flow_status);
        fflush(stdout);
    }
    #endif

    int p = ntohl(tcp->th_seq) - tflow->sc_init_seq;
    if(p < 0) // this should not be possible, skip it!
        return; 

    if(tflow->flow_status == FLOW_HTTP_RESP_HEADER_WAIT || tflow->flow_status == FLOW_HTTP_RESP_MZ_WAIT) {
        #ifdef PE_DEBUG
        if(debug_level >= VERY_VERY_VERBOSE) {
            printf("p=%d, th_seq=%u, sc_init_seq=%u\n",p,ntohl(tcp->th_seq),tflow->sc_init_seq);
            fflush(stdout);
        }
        #endif


        if(p+payload_size < tflow->sc_payload_capacity) {
            // memcpy(&(tflow->sc_payload[p]), payload, MIN(tflow->sc_payload_capacity - p - 1, payload_size));
            memcpy(&(tflow->sc_payload[p]), payload, payload_size);
            seq_list_insert(tflow->sc_seq_list, ntohl(tcp->th_seq), payload_size); 

            #ifdef PE_DEBUG
            if(debug_level >= VERY_VERY_VERBOSE) {
                printf("Called memcpy...\n",tflow->anon_cs_key, tflow->sc_payload);
                // printf("Payload for %s:\n%s\n\n",tflow->anon_cs_key, tflow->sc_payload);
                printf("SC SEQ LIST = ");
                seq_list_print(tflow->sc_seq_list);
                fflush(stdout);
            }
            #endif

            if(p+payload_size > tflow->sc_payload_size) // updates where the sc_payload ends
                tflow->sc_payload_size = p+payload_size;

            #ifdef PE_DEBUG
            if(debug_level >= VERY_VERY_VERBOSE) {
                printf("tflow->sc_payload_size = %d\n",tflow->sc_payload_size);
                fflush(stdout);
            }
            #endif

        }

        return;
    }
    else if(tflow->flow_status == FLOW_HTTP_RESP_MZ_FOUND) {
        if(p+payload_size < tflow->sc_payload_capacity) {
            memcpy(&(tflow->sc_payload[p]), payload, payload_size);
            seq_list_insert(tflow->sc_seq_list, ntohl(tcp->th_seq), payload_size);

            if(p+payload_size > tflow->sc_payload_size) // updates where the sc_payload ends
                tflow->sc_payload_size = p+payload_size;

            #ifdef PE_DEBUG
            if(debug_level >= VERY_VERY_VERBOSE) {
                printf("Updated tflow paylaod without reallocating memory: %s\n",tflow->anon_cs_key);
                printf("SC SEQ LIST = ");
                seq_list_print(tflow->sc_seq_list);
                fflush(stdout);
            }
            #endif

        }
        else {


            int realloc_size = MAX(REALLOC_SC_PAYLOAD, payload_size);

            if(p+payload_size > tflow->sc_payload_capacity+realloc_size) 
                // something wrong here... probably extreme packet reordering or loss... skip it!
                return;

            #ifdef PE_DEBUG
            if(debug_level >= VERY_VERY_VERBOSE) {
                printf("Need to reallocate %d tflow paylaod memory: %s\n",realloc_size,tflow->anon_cs_key);
                printf("Current payload capacity = %d\n",tflow->sc_payload_capacity);
                printf("Payload pointer = %p\n",tflow->sc_payload);
                fflush(stdout);
            }
            #endif

            tflow->sc_payload_capacity += realloc_size;

            #ifdef PE_DEBUG
            if(debug_level >= VERY_VERY_VERBOSE) {
                printf("New capacity = %d\n",tflow->sc_payload_capacity);
                fflush(stdout);
            }
            #endif

            // tflow->sc_payload = realloc(tflow->sc_payload, tflow->sc_payload_capacity+1);

            if(tflow->sc_payload!=NULL) {
                char *tmp_ptr = tflow->sc_payload;
                tflow->sc_payload = NULL;
                tflow->sc_payload = malloc((tflow->sc_payload_capacity+1)*sizeof(char));

                if(tflow->sc_payload == NULL)
                    return;

                #ifdef PE_DEBUG
                if(debug_level >= VERY_VERY_VERBOSE) {
                    printf("Reallocated tflow paylaod memory: %s\n",tflow->anon_cs_key);
                    printf("Old payload pointer = %p\n",tmp_ptr);
                    printf("New payload pointer = %p\n",tflow->sc_payload);
                    fflush(stdout);
                }
                #endif

                memset(tflow->sc_payload, 0, (tflow->sc_payload_capacity+1));

                #ifdef PE_DEBUG
                if(debug_level >= VERY_VERY_VERBOSE) {
                    printf("Initialized new paylaod memory: %s\n",tflow->anon_cs_key);
                    fflush(stdout);
                }
                #endif

                memcpy(tflow->sc_payload, tmp_ptr, tflow->sc_payload_size);

                #ifdef PE_DEBUG
                if(debug_level >= VERY_VERY_VERBOSE) {
                    printf("Copied paylaod memory: %s\n",tflow->anon_cs_key);
                    printf("Freeing old payload pointer = %p\n",tmp_ptr);
                    fflush(stdout);
                }
                #endif

                free(tmp_ptr);

                #ifdef PE_DEBUG
                if(debug_level >= VERY_VERY_VERBOSE) {
                    printf("Freed old payload memory: %s\n",tflow->anon_cs_key);
                    fflush(stdout);
                }
                #endif

            }


            #ifdef PE_DEBUG
            if(debug_level >= VERY_VERY_VERBOSE) {
                printf("Initialized new paylaod memory: %s\n",tflow->anon_cs_key);
                fflush(stdout);
            }
            #endif

            memcpy(&(tflow->sc_payload[p]), payload, payload_size);
            seq_list_insert(tflow->sc_seq_list, ntohl(tcp->th_seq), payload_size);

            if(p+payload_size > tflow->sc_payload_size) // updates where the sc_payload ends
                tflow->sc_payload_size = p+payload_size;


            #ifdef PE_DEBUG
            if(debug_level >= VERY_VERY_VERBOSE) {
                printf("Updated tflow paylaod memory reallocation: %s\n",tflow->anon_cs_key);
                printf("SC SEQ LIST = ");
                seq_list_print(tflow->sc_seq_list);
                fflush(stdout);
            }
            #endif

        } 

        #ifdef PE_DEBUG
        if(debug_level >= VERY_VERY_VERBOSE) {
            printf("MZ %s has new payload size %d\n",tflow->anon_cs_key, tflow->sc_payload_size);
            fflush(stdout);
        }
        #endif
    }
}


void reset_flow_payload(struct tcp_flow *tflow) {
    if(tflow->sc_payload==NULL)
        return;

    free(tflow->sc_payload);
    tflow->sc_payload = NULL;

    if(tflow->sc_seq_list != NULL) {
        seq_list_destroy(tflow->sc_seq_list);
        free(tflow->sc_seq_list);
    }
    tflow->sc_seq_list = NULL;
}


// using thing instead of sprintf because sprintf was causing strange problems...
void get_key(char *key, const char* pkt_src, const char *pkt_dst) {
    key[0]='\0';
    strcat(key,pkt_src);
    strcat(key,"-");
    strcat(key,pkt_dst);
    // printf("key = %s\n",key);

    return;    
}


/* itoa:  convert n to characters in s */
void itoa(int n, char *s) {
     int i, sign;
 
     if ((sign = n) < 0)  /* record sign */
         n = -n;          /* make n positive */
     i = 0;
     do {       /* generate digits in reverse order */
         s[i++] = n % 10 + '0';   /* get next digit */
     } while ((n /= 10) > 0);     /* delete it */
     if (sign < 0)
         s[i++] = '-';
     s[i] = '\0';
     reverse(s);
}


/* reverse:  reverse string s in place */
void reverse(char *s) {
     int i, j;
     char c;
 
     for (i = 0, j = strlen(s)-1; i<j; i++, j--) {
         c = s[i];
         s[i] = s[j];
         s[j] = c;
     }
}
