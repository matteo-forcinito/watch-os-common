// Baseline arc rasterizer retained only for pixel-equivalence regression.
static inline void referenceArc(Canvas& c, float cx, float cy, float r0, float r1, float start, float sweep,
                           uint16_t col) {
  using namespace round_detail;
  if (r1 <= 0 || sweep <= 0) return;
  if (r0 < 0) r0 = 0;
  const bool full = sweep >= 360.f;
  int yTop = (int)floorf(cy - r1 - 1), yBot = (int)ceilf(cy + r1 + 1);
  if (yTop < c.top) yTop = c.top;
  if (yBot > c.h - 1) yBot = c.h - 1;
  const float ro2 = (r1 + 0.5f) * (r1 + 0.5f);
  const float ri2 = r0 > 0.5f ? (r0 - 0.5f) * (r0 - 0.5f) : -1.f;
  for (int y = yTop; y <= yBot; y++) {
    const float dy = y + 0.5f - cy;
    const float dy2 = dy * dy;
    if (dy2 > ro2) continue;
    const float xo = sqrtf(ro2 - dy2);
    // The inner hole on this row, if any.
    float xi = -1.f;
    if (ri2 > 0 && dy2 < ri2) xi = sqrtf(ri2 - dy2);
    int x0 = (int)floorf(cx - xo), x1 = (int)ceilf(cx + xo);
    if (x0 < 0) x0 = 0;
    if (x1 > c.w - 1) x1 = c.w - 1;
    for (int x = x0; x <= x1; x++) {
      const float dx = x + 0.5f - cx;
      if (xi > 1.5f && dx > -xi + 1.f && dx < xi - 1.f) {   // skip the hole in one go
        x = (int)floorf(cx + xi - 1.f);
        continue;
      }
      const float d = sqrtf(dx * dx + dy2);
      const int a = ringCover(d, r0, r1);
      if (!a) continue;
      if (!full && !angleOnArc(angleAt(0, 0, dx, dy), start, sweep)) continue;
      blendPx(c, x, y, col, a);
    }
  }
}
