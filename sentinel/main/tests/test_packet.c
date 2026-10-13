#include "packet.h"
#include <string.h>
#define CHECK(test) do { if (!(test)) return __LINE__; } while (0)
#ifdef SENTINEL_EMULATOR
size_t strlen(const char *s) { size_t n=0; while(s[n]) n++; return n; }
void *memcpy(void *to,const void *from,size_t n) { unsigned char *a=to; const unsigned char *b=from; for(size_t i=0;i<n;i++) a[i]=b[i]; return to; }
int memcmp(const void *a,const void *b,size_t n) { const unsigned char *x=a,*y=b; for(size_t i=0;i<n;i++) if(x[i]!=y[i]) return x[i]-y[i]; return 0; }
#endif
int test_packet(void)
{
    uint8_t frame[42]={0};
    uint32_t own=0x0101a8c0, sender=0x0201a8c0, result=0;
    uint8_t mac[6]={2,3,4,5,6,7}, found[6];
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
