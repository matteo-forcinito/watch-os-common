#ifndef CORE_ORIENTATION_H
#define CORE_ORIENTATION_H
/* Pure geometry: logical UI -> native panel, clockwise quarter turns. */
static inline void orientation_native(int r,int w,int h,int x,int y,int *nx,int *ny) {
    switch(r) {
    case 1:*nx=y;*ny=h-1-x;break;
    case 2:*nx=w-1-x;*ny=h-1-y;break;
    case 3:*nx=w-1-y;*ny=x;break;
    default:*nx=x;*ny=y;break;
    }
}
static inline void orientation_logical(int r,int w,int h,int x,int y,int *lx,int *ly) {
    switch(r) {
    case 1:*lx=h-1-y;*ly=x;break;
    case 2:*lx=w-1-x;*ly=h-1-y;break;
    case 3:*lx=y;*ly=w-1-x;break;
    default:*lx=x;*ly=y;break;
    }
}
#endif
