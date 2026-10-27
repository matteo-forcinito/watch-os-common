#pragma once
#include <initializer_list>
#include "upstream/layout_square.h"
/* Same upstream typography, sprites and interactions, reflowed for a wide panel. */
static inline SquareSpec specLandscape(int width,int height) {
    auto s=specAmoled18();s.name="landscape";s.w=width;s.h=height;
    s.cols=width>=420?3:2;s.gap=8;s.pad=12;s.headerH=44;
    s.tileW=(width-2*s.pad-(s.cols-1)*s.gap)/s.cols;
    s.tileH=(height-s.headerH-s.gap-16)/2;
    s.tileScale=3;s.cardScale=3;s.noticeScale=4;
    s.posTop=60;s.posBottom=32;s.btnR=28;s.btnFromBottom=38;
    return s;
}
