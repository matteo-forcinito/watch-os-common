#ifndef SOLCO_LAYOUT_H
#define SOLCO_LAYOUT_H
#include <stdbool.h>
typedef struct {int x,y,w,h;} solco_rect_t;
typedef struct {bool landscape;solco_rect_t deck[2],pad[8],disc;} solco_layout_t;
/* Controls use a 360-unit canvas; platter geometry stays in physical pixels. */
static inline solco_layout_t solco_layout_for(int width,int height,bool round) {
    solco_layout_t out={0};out.landscape=!round && width>height;
    for(int i=0;i<2;i++) {
        int w=out.landscape?168:round?296:344;
        out.deck[i]=(solco_rect_t){out.landscape?8+i*176:(360-w)/2,
            out.landscape?44:round?54+i*88:44+i*94,w,out.landscape?174:round?84:90};
    }
    for(int i=0;i<8;i++) {
        int cols=out.landscape?4:2;
        out.pad[i]=(solco_rect_t){(round?36:8)+(i%cols)*(out.landscape?87:round?148:176),
            (out.landscape?76:round?76:54)+(i/cols)*(out.landscape?108:round?54:64),
            out.landscape?82:round?140:168,out.landscape?96:round?50:60};
    }
    int size=out.landscape?height-16:width;
    out.disc=(solco_rect_t){(width-size)/2,(height-size)/2,size,size};
    return out;
}
#endif
