#include "studio.h"
#include <stdio.h>
int main(void)
{
    studio_t studio;
    if (!studio_init(&studio)) return 1;
    const int banks[]={32,0,33,5};
    const char *names[]={"kick-A","kick-B","tom-A","tom-B"};
    for (int i=0;i<4;i++)
    {
        char path[160];
        snprintf(path,sizeof(path),"progress-files/solco-%s.pcm",names[i]);
        FILE *file=fopen(path,"wb");
        if (!file) return 2;
        studio_sample_t *sample=&studio.bank[banks[i]];
        if (fwrite(sample->pcm,sizeof(int16_t),sample->frames,file)!=sample->frames) return 3;
        fclose(file);
    }
    studio_free(&studio); return 0;
}
