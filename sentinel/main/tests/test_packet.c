#include "packet.h"
#include <string.h>
#define CHECK(test) do { if (!(test)) return __LINE__; } while (0)
#ifdef SENTINEL_EMULATOR
size_t strlen(const char *s) { size_t n=0; while(s[n]) n++; return n; }
void *memcpy(void *to,const void *from,size_t n) { unsigned char *a=to; const unsigned char *b=from; for(size_t i=0;i<n;i++) a[i]=b[i]; return to; }
int memcmp(const void *a,const void *b,size_t n) { const unsigned char *x=a,*y=b; for(size_t i=0;i<n;i++) if(x[i]!=y[i]) return x[i]-y[i]; return 0; }
void *memset(void *to,int value,size_t n) { unsigned char *a=to; for(size_t i=0;i<n;i++) a[i]=(unsigned char)value; return to; }
char *strchr(const char *s,int c) { for(;;s++) { if(*s==(char)c) return (char *)s; if(!*s) return 0; } }
int strncmp(const char *a,const char *b,size_t n) { for(size_t i=0;i<n;i++) { if(a[i]!=b[i]) return (unsigned char)a[i]-(unsigned char)b[i]; if(!a[i]) return 0; } return 0; }
int strcmp(const char *a,const char *b) { while(*a && *a==*b) { a++; b++; } return (unsigned char)*a-(unsigned char)*b; }
#endif
static int test_identification(void)
{
    uint8_t packet[128];
    char name[64];
    uint16_t id = 0;
    uint32_t ip; uint8_t *octet = (uint8_t *)&ip;
    octet[0]=192; octet[1]=168; octet[2]=1; octet[3]=23;
    /* PTR query: header, 23.1.168.192.in-addr.arpa, type 12, class 1. */
    size_t length = sn_dns_ptr_query(packet,sizeof(packet),0x1234,ip);
    static const uint8_t query[] = {0x12,0x34,1,0,0,1,0,0,0,0,0,0,2,'2','3',1,'1',3,'1','6','8',3,'1','9','2',
        7,'i','n','-','a','d','d','r',4,'a','r','p','a',0,0,12,0,1};
    CHECK(length == sizeof(query)); CHECK(memcmp(packet,query,sizeof(query)) == 0);
    CHECK(sn_dns_ptr_query(packet,20,1,ip) == 0);
    /* Answer: the query echoed, then a PTR record pointing back to the question (0xc00c). */
    uint8_t answer[128];
    memcpy(answer,query,sizeof(query));
    answer[2] = 0x81; answer[3] = 0x80; answer[7] = 1;
    static const uint8_t record[] = {0xc0,0x0c,0,12,0,1,0,0,0,60,0,11,5,'p','h','o','n','e',3,'l','a','n',0};
    memcpy(answer+sizeof(query),record,sizeof(record));
    size_t total = sizeof(query)+sizeof(record);
    CHECK(sn_dns_ptr_answer(answer,total,&id,name,sizeof(name))); CHECK(id == 0x1234);
    CHECK(strcmp(name,"phone.lan") == 0);
    CHECK(!sn_dns_ptr_answer(answer,total-1,&id,name,sizeof(name)));   /* truncated rdata */
    CHECK(!sn_dns_ptr_answer(answer,total,&id,name,6));                /* name does not fit */
    answer[3] = 0x83; CHECK(!sn_dns_ptr_answer(answer,total,&id,name,sizeof(name))); answer[3] = 0x80; /* NXDOMAIN */
    answer[2] = 0x01; CHECK(!sn_dns_ptr_answer(answer,total,&id,name,sizeof(name))); answer[2] = 0x81; /* a query */
    answer[sizeof(query)+4] = 0x80;  /* mDNS cache-flush bit on class IN */
    CHECK(sn_dns_ptr_answer(answer,total,&id,name,sizeof(name)));
    /* Compression loop (pointer to itself) and invalid bytes are rejected. */
    answer[sizeof(query)+12] = 0xc0; answer[sizeof(query)+13] = (uint8_t)(sizeof(query)+12);
    CHECK(!sn_dns_ptr_answer(answer,total,&id,name,sizeof(name)));
    answer[sizeof(query)+12] = 5; answer[sizeof(query)+13] = '\n';
    CHECK(!sn_dns_ptr_answer(answer,total,&id,name,sizeof(name)));
    CHECK(!sn_dns_ptr_answer(answer,11,&id,name,sizeof(name)));
    /* NetBIOS node status: request layout, then a reply with a group and a unique name. */
    CHECK(sn_nbstat_query(packet,sizeof(packet),0x0042) == 50);
    CHECK(packet[0] == 0 && packet[1] == 0x42 && packet[12] == 32 && packet[13] == 'C' && packet[14] == 'K');
    CHECK(packet[44] == 'A' && packet[45] == 0 && packet[47] == 0x21 && packet[49] == 1);
    uint8_t reply[128] = {0};
    memcpy(reply,packet,46); reply[2] = 0x84; reply[5] = 0; reply[7] = 1;
    size_t at = 46;
    reply[at+1] = 0x21; reply[at+3] = 1; reply[at+9] = 1+2*18; at += 10;
    reply[at++] = 2;
    memcpy(reply+at,"WORKGROUP      ",15); reply[at+15] = 0; reply[at+16] = 0x84; at += 18;
    memcpy(reply+at,"DESKTOP-42     ",15); reply[at+15] = 0; reply[at+16] = 0x04; at += 18;
    CHECK(sn_nbstat_name(reply,at,0x0042,name,sizeof(name))); CHECK(strcmp(name,"DESKTOP-42") == 0);
    CHECK(!sn_nbstat_name(reply,at,0x0043,name,sizeof(name)));
    CHECK(!sn_nbstat_name(reply,at-30,0x0042,name,sizeof(name)));
    reply[at-18] = 7; CHECK(!sn_nbstat_name(reply,at,0x0042,name,sizeof(name)));
    /* SSDP LOCATION: only the responding host, valid port and printable path. */
    uint16_t port = 0;
    char path[64];
    octet[3] = 7;
    const char *ssdp = "HTTP/1.1 200 OK\r\nCACHE-CONTROL: max-age=1800\r\nLocation: http://192.168.1.7:49152/desc.xml\r\n\r\n";
    CHECK(sn_location(ssdp,ip,&port,path,sizeof(path))); CHECK(port == 49152); CHECK(strcmp(path,"/desc.xml") == 0);
    const char *other = "HTTP/1.1 200 OK\r\nLOCATION: http://192.168.1.70/desc.xml\r\n\r\n";
    CHECK(!sn_location(other,ip,&port,path,sizeof(path)));
    const char *plain = "HTTP/1.1 200 OK\r\nLOCATION: http://192.168.1.7/d.xml\r\n\r\n";
    CHECK(sn_location(plain,ip,&port,path,sizeof(path))); CHECK(port == 80);
    const char *big = "HTTP/1.1 200 OK\r\nLOCATION: http://192.168.1.7:99999/d.xml\r\n\r\n";
    CHECK(!sn_location(big,ip,&port,path,sizeof(path)));
    const char *tls = "HTTP/1.1 200 OK\r\nLOCATION: https://192.168.1.7/d.xml\r\n\r\n";
    CHECK(!sn_location(tls,ip,&port,path,sizeof(path)));
    const char *body = "HTTP/1.1 200 OK\r\n\r\nLOCATION: http://192.168.1.7/d.xml\r\n";
    CHECK(!sn_location(body,ip,&port,path,sizeof(path)));
    char server[32];
    CHECK(sn_http_header("HTTP/1.0 200 OK\r\nserver: lighttpd/1.4  \r\n\r\n","Server",server,sizeof(server)));
    CHECK(strcmp(server,"lighttpd/1.4") == 0);
    /* XML/HTML values: entities, whitespace, truncation without split UTF-8. */
    const char *xml = "<root><device><friendlyName>\n  Sala &amp; TV </friendlyName><modelName>Q60</modelName></device></root>";
    CHECK(sn_xml_value(xml,"friendlyName",name,sizeof(name))); CHECK(strcmp(name,"Sala & TV") == 0);
    CHECK(sn_xml_value(xml,"modelname",name,sizeof(name))); CHECK(strcmp(name,"Q60") == 0);
    CHECK(!sn_xml_value(xml,"manufacturer",name,sizeof(name)));
    CHECK(sn_xml_value("<TITLE lang=it>Caff\xc3\xa8</TITLE>","title",name,6)); CHECK(strcmp(name,"Caff") == 0);
    CHECK(!sn_xml_value("<title></title>","title",name,sizeof(name)));
    CHECK(!sn_xml_value("<title>a\x01</title>","title",name,sizeof(name)));
    return 0;
}

int test_packet(void)
{
    int failed = test_identification();
    if (failed) return failed;
    uint8_t frame[42]={0};
    uint32_t own=0x0101a8c0, sender=0x0201a8c0, result=0;
    uint8_t mac[6]={2,3,4,5,6,7}, found[6];
    uint8_t other[6]={2,3,4,5,6,8}, unknown[6]={0};
    CHECK(sn_proxy_arp(sender,own,mac,mac));
    CHECK(!sn_proxy_arp(own,own,mac,mac));
    CHECK(!sn_proxy_arp(sender,own,other,mac));
    CHECK(!sn_proxy_arp(sender,own,mac,unknown));
    CHECK(!sn_proxy_arp(sender,own,NULL,mac));
    memcpy(frame+6,mac,6); frame[12]=8; frame[13]=6;
    frame[15]=1; frame[16]=8; frame[18]=6; frame[19]=4; frame[21]=2;
    memcpy(frame+22,mac,6); memcpy(frame+28,&sender,4); memcpy(frame+38,&own,4);
    CHECK(sn_arp_reply(frame,42,own,&result,found)); CHECK(result==sender); CHECK(memcmp(found,mac,6)==0);
    CHECK(!sn_arp_reply(frame,41,own,&result,found));
    CHECK(!sn_arp_reply(NULL,42,own,&result,found));
    CHECK(!sn_arp_reply(frame,42,own,NULL,found));
    CHECK(!sn_arp_reply(frame,42,own+1,&result,found));
    frame[21]=1; CHECK(!sn_arp_reply(frame,42,own,&result,found)); frame[21]=2;
    frame[18]=5; CHECK(!sn_arp_reply(frame,42,own,&result,found)); frame[18]=6;
    frame[22]=3; CHECK(!sn_arp_reply(frame,42,own,&result,found)); frame[22]=2;
    frame[6]=4; CHECK(!sn_arp_reply(frame,42,own,&result,found));
    CHECK(sn_text_valid("Casa",4)); CHECK(!sn_text_valid("Casa",3));
    CHECK(sn_text_valid("Caff\xc3\xa8",6)); CHECK(!sn_text_valid("\xc0\x80",8));
    CHECK(!sn_text_valid("\xed\xa0\x80",8)); CHECK(!sn_text_valid("\xf4\x90\x80\x80",8));
    CHECK(!sn_text_valid("\xf0\x9f",8)); CHECK(!sn_text_valid("a\nb",8));
    CHECK(sn_text_valid("\xf0\x9f\x8c\x90",4));
    const char *valid="{\"a\":[1,{\"b\":\"x\\\"{[]}\"}]}";
    CHECK(sn_json_safe(valid,strlen(valid)));
    CHECK(!sn_json_safe("{]",2)); CHECK(!sn_json_safe("[",1));
    CHECK(!sn_json_safe("\"a\nb\"",5));
    CHECK(!sn_json_safe("{}\0[]",5)); CHECK(!sn_json_safe("\"unterminated",13));
    const char *nul = "{\"id\":\"x\\u0000y\"}";
    CHECK(!sn_json_safe(nul,strlen(nul)));
    const char *literal = "{\"note\":\"x\\\\u0000y\"}";
    CHECK(sn_json_safe(literal,strlen(literal)));
    CHECK(!sn_json_safe("[[[[[[[[[[[[[]]]]]]]]]]]]]",26));
    return 0;
}
#ifdef SENTINEL_EMULATOR
__asm__(".global _start\n_start:\ncall test_packet\nebreak\nj .\n");
#else
int main(void) { return test_packet(); }
#endif
