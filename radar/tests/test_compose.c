/* Radar picture maths tests: palette, tile placement with clipping and scale, blending, nearest rain. */
#include "compose.h"

#define CHECK(condition) do { if (!(condition)) return __LINE__; } while (0)

#define W 40
#define H 30

static radar_palette_t s_palette;
static uint8_t s_tile[256 * 256 * 4];
static uint16_t s_base[W * H], s_out[W * H];
static uint8_t s_frame[W * H];

static void fill_tile(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    for (int i = 0; i < 256 * 256; i++)
    {
        s_tile[i * 4] = r;
        s_tile[i * 4 + 1] = g;
        s_tile[i * 4 + 2] = b;
        s_tile[i * 4 + 3] = a;
    }
}

static int near(float a, float b, float tolerance) { return a - b < tolerance && b - a < tolerance; }

int radar_tests(void)
{
    /* Palette: transparent is 0, colours are deduplicated, the table never overflows. */
    radar_palette_init(&s_palette);
    CHECK(radar_palette_index(&s_palette, 0x11223300) == 0);
    uint8_t blue = radar_palette_index(&s_palette, 0x0088CCFF);
    CHECK(blue == 1 && radar_palette_index(&s_palette, 0x0088CCFF) == 1 && s_palette.count == 2);
    CHECK(s_palette.alpha[blue] == 0xFF && s_palette.rgb565[blue] == ((0x00 >> 3) << 11 | (0x88 >> 2) << 5 | (0xCC >> 3)));
    for (uint32_t i = 0; i < 400; i++)
        radar_palette_index(&s_palette, (i << 8) | 0xFF);
    CHECK(s_palette.count == 256 && radar_palette_index(&s_palette, 0xABCDEFFF) == 0);

    /* Base: a tile half outside the view fills only the overlapping part. View origin (250, 100). */
    for (int i = 0; i < W * H; i++)
        s_base[i] = 0x1234;
    fill_tile(255, 255, 255, 255); /* white land -> darkest */
    radar_blit_base(s_base, W, H, 250, 100, s_tile, 0, 0); /* tile 0 covers x 0..255: screen x 0..5 */
    CHECK(s_base[0] != 0x1234 && s_base[5] != 0x1234 && s_base[6] == 0x1234);
    CHECK(s_base[0] == ((16 >> 3) << 11 | (18 >> 2) << 5 | (24 >> 3)));
    fill_tile(170, 211, 223, 255); /* OSM water */
    radar_blit_base(s_base, W, H, 250, 100, s_tile, 1, 0); /* tile 1: x 256.. -> screen 6.. */
    uint16_t water = s_base[6];
    CHECK(water != 0x1234 && (water & 31) > ((water >> 11) & 31)); /* bluer than red */

    /* Radar one zoom lower (shift 1): one source pixel covers 2x2 screen pixels. */
    radar_palette_init(&s_palette);
    for (int i = 0; i < W * H; i++)
        s_frame[i] = 0;
    fill_tile(0, 0, 0, 0);
    /* Source pixel (130, 50) of tile (0,0) at the lower zoom = base global (260..261, 100..101) = screen (10..11, 0..1). */
    uint8_t *px = s_tile + (50 * 256 + 130) * 4;
    px[0] = 0;
    px[1] = 0x88;
    px[2] = 0xCC;
    px[3] = 0xFF;
    radar_blit_radar(s_frame, W, H, 250, 100, 1, s_tile, 0, 0, &s_palette);
    CHECK(s_frame[10] == 1 && s_frame[11] == 1 && s_frame[W + 10] == 1 && s_frame[W + 11] == 1);
    CHECK(s_frame[9] == 0 && s_frame[12] == 0 && s_frame[2 * W + 10] == 0);

    /* Compose: rain pixels take the rain colour (opaque), others keep the base. */
    radar_compose_frame(s_base, s_frame, &s_palette, s_out, W * H);
    CHECK(s_out[10] != s_base[10] && s_out[10] != s_palette.rgb565[1] && s_out[9] == s_base[9]);
    CHECK((s_out[10] & 31) > (s_base[10] & 31)); /* bluer than the dark map under it */

    /* Nearest rain from the centre (20, 15): pixel (11, 1) area, up-left = north-west. */
    float distance, bearing;
    CHECK(radar_nearest_rain(s_frame, &s_palette, W, H, 20, 15, 100, &distance, &bearing));
    CHECK(near(distance, radar_sqrt(9 * 9 + 14 * 14), 0.01f) && bearing > 270 && bearing < 360);
    CHECK(!radar_nearest_rain(s_frame, &s_palette, W, H, 20, 15, 256, &distance, &bearing));
    CHECK(radar_intensity_at(s_frame, &s_palette, W, H, 10, 0, 1) > 0.5f);
    CHECK(radar_intensity_at(s_frame, &s_palette, W, H, 30, 20, 2) == 0);

    /* Bearings: screen up = north, right = east. */
    CHECK(near(radar_bearing(0, -5), 0, 0.5f) && near(radar_bearing(5, 0), 90, 0.5f));
    CHECK(near(radar_bearing(0, 5), 180, 0.5f) && near(radar_bearing(-5, 0), 270, 0.5f));
    CHECK(near(radar_bearing(-5, 5), 225, 0.5f) && near(radar_bearing(3, -4), 36.87f, 0.5f));
    CHECK(near(radar_sqrt(2.0f), 1.41421f, 0.001f) && radar_sqrt(0) == 0);
    return 0;
}

#ifdef RADAR_EMULATOR_ENTRY
__attribute__((naked)) void _start(void)
{
    /* Full-size ebreak: the runner looks for the 4-byte instruction. */
    __asm__ volatile(".option push\n .option norvc\n call radar_tests\n ebreak\n .option pop\n");
}
#else
int main(void)
{
    return radar_tests();
}
#endif
