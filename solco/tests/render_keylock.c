/* Offline QA renderer for owned PCM16 stereo 44.1 kHz voice fixtures. */
#include "vinyl.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
typedef struct { int16_t *pcm; uint32_t frames; } source_t;
static bool sample(void *data, int64_t at, int16_t stereo[2])
{
    source_t *s=data;
    if(at<0 || at>=s->frames) return false;
    stereo[0]=s->pcm[at*2];stereo[1]=s->pcm[at*2+1];return true;
}
int main(int argc,char **argv)
{
    assert(argc==5);
    FILE *in=fopen(argv[1],"rb");assert(in);
    unsigned char header[44];assert(fread(header,1,44,in)==44);
    assert(header[22]==2 && header[34]==16);
    fseek(in,0,SEEK_END);long bytes=ftell(in)-44;rewind(in);fseek(in,44,SEEK_SET);
    source_t source={malloc((size_t)bytes),(uint32_t)(bytes/4)};assert(source.pcm);
    assert(fread(source.pcm,1,(size_t)bytes,in)==(size_t)bytes);fclose(in);
    int rate=atoi(argv[3]);assert(rate);
    solco_keylock_t *lock=calloc(1,sizeof(*lock));assert(lock);
    solco_vinyl_t v={0};v.keylock=lock;v.keylock_enabled=atoi(argv[4])!=0;
    v.play_speed=v.speed=SOLCO_RATE_FROM_Q16(rate);
    v.cursor=rate<0?(int64_t)(source.frames-1)*SOLCO_SAMPLE_ONE:0;
    uint32_t frames=(uint32_t)((int64_t)source.frames*65536/abs(rate));
    uint32_t count=frames*4;
    for(int i=0;i<4;++i){header[4+i]=(unsigned char)((count+36)>>(i*8));header[40+i]=(unsigned char)(count>>(i*8));}
    FILE *out=fopen(argv[2],"wb");assert(out);assert(fwrite(header,1,44,out)==44);
    for(uint32_t at=0;at<frames;at+=256)
    {
        int16_t pcm[512];bool missing;size_t block=frames-at<256?frames-at:256;
        solco_vinyl_render(&v,pcm,block,true,source.frames,false,sample,&source,&missing);
        assert(!missing);assert(fwrite(pcm,4,block,out)==block);
    }
    assert(fclose(out)==0);free(source.pcm);free(lock);
    printf("Rendered %s: %lu frames, rate %d, keylock %s\n",argv[2],(unsigned long)frames,rate,argv[4]);return 0;
}
