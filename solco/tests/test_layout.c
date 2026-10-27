#include "layout.h"
#include <assert.h>
#include <stdio.h>
static int overlap(solco_rect_t a,solco_rect_t b) {
 return a.x<b.x+b.w && b.x<a.x+a.w && a.y<b.y+b.h && b.y<a.y+a.h;
}
int main(void) {
 const int sizes[][3]={{448,368,0},{368,448,0},{360,360,1},{466,466,1},{480,480,0},{600,450,0}};
 for(unsigned n=0;n<sizeof(sizes)/sizeof(sizes[0]);n++) {
  solco_layout_t l=solco_layout_for(sizes[n][0],sizes[n][1],sizes[n][2]!=0);
  assert(!overlap(l.deck[0],l.deck[1]));
  for(int i=0;i<8;i++) {
   assert(l.pad[i].x>=0 && l.pad[i].x+l.pad[i].w<=360);
   assert(l.pad[i].y>=0 && l.pad[i].y+l.pad[i].h<=312);
   for(int j=0;j<i;j++)assert(!overlap(l.pad[i],l.pad[j]));
  }
  assert(l.disc.x>=0 && l.disc.y>=0);
  assert(l.disc.x+l.disc.w<=sizes[n][0] && l.disc.y+l.disc.h<=sizes[n][1]);
  assert(l.disc.w==l.disc.h);
 }
 puts("SOLCO LAYOUT PASS: six display geometries, no overlapping controls");
}
