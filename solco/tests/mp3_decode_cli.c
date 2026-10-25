#include "mp3.h"
#include <stdlib.h>
static bool progress(int percent, void *context)
{
    (void)percent;
    return context == NULL;
}
int main(int argc, char **argv)
{
    if (argc < 3) return 2;
    FILE *input = fopen(argv[1], "rb"), *output = fopen(argv[2], "wb");
    if (!input || !output) return 3;
    char error[96];
    uint64_t limit = argc > 3 ? strtoull(argv[3], NULL, 10) : 100000000;
    bool ok = solco_mp3_decode(input, output, limit, progress, argc > 4 ? error : NULL, error, sizeof(error));
    fclose(input); fclose(output);
    if (!ok) fprintf(stderr, "%s\n", error);
    return ok ? 0 : 1;
}
