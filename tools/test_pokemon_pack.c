#include "orbi/pokemon_pack.h"
#include <assert.h>
#include <stdio.h>

int main(int argc, char **argv)
{
    assert(argc == 3);
    pokemon_pack_t p = {0};
    uint32_t frame[1024];
    if (argv[2][0] == 'b')
    {
        assert(!pokemon_pack_open(&p, argv[1]));
        assert(!p.file);
    }
    else if (argv[2][0] == 'a')
    {
        assert(pokemon_pack_open(&p, argv[1]));
        unsigned found = 0;
        for (uint16_t dex = 1; dex <= 151; dex++) for (int shiny = 0; shiny < 2; shiny++)
        {
            if (!pokemon_pack_read(&p, dex, shiny != 0, 0, frame)) continue;
            found++;
            bool visible = false;
            for (unsigned i = 0; i < 1024; i++) visible |= frame[i] != 0;
            assert(visible);
            for (uint32_t f = 1; f < 4; f++) assert(pokemon_pack_read(&p, dex, shiny != 0, f, frame));
        }
        assert(found == p.count);
        pokemon_pack_close(&p);
    }
    else
    {
        assert(pokemon_pack_open(&p, argv[1]));
        assert(!pokemon_pack_open(&p, argv[1])); /* don't leak a live handle */
        assert(pokemon_pack_read(&p, 25, false, 0, frame));
        assert(frame[0] == 0 && frame[31] == 0xFFFF0000u);
        assert(pokemon_pack_read(&p, 25, false, 999, frame));
        assert(frame[0] == 0xFF00FF00u && frame[31] == 0xFF00FF00u);
        assert(!pokemon_pack_read(&p, 25, true, 0, frame));
        assert(frame[0] == 0xFF00FF00u); /* failure doesn't corrupt cached art */
        pokemon_pack_close(&p);
        pokemon_pack_close(&p);
        assert(!pokemon_pack_read(&p, 25, false, 0, frame));
    }
    puts("POKEMON_PACK_PASS");
    return 0;
}
