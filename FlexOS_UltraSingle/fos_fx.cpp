// #############################################################
// ##  Flex OS UltraSingle  -  fos_fx.cpp
// #############################################################
#include "fos_fx.h"
#include "fos_gfx.h"

#define FX_BAND 32

void fxWipe(bool up){
  if(up){
    for(int16_t y = SCR_H - FX_BAND; y >= 0; y -= FX_BAND) gfxWallRect(0, y, SCR_W, FX_BAND);
  } else {
    for(int16_t y = 0; y < SCR_H; y += FX_BAND) gfxWallRect(0, y, SCR_W, FX_BAND);
  }
}

// Interpolacion lineal entera entre a y b con peso k/n.
static inline int16_t lerpi(int16_t a, int16_t b, uint8_t k, uint8_t n){
  return (int16_t)(a + ((int32_t)(b - a) * k) / n);
}

void fxOpenApp(int16_t ix, int16_t iy, uint8_t is){
  const uint8_t STEPS = 5;
  for(uint8_t k = 1; k <= STEPS; k++){
    int16_t x = lerpi(ix, 0, k, STEPS);
    int16_t y = lerpi(iy, 0, k, STEPS);
    int16_t w = lerpi(is, SCR_W, k, STEPS);
    int16_t h = lerpi(is, SCR_H, k, STEPS);
    int16_t r = lerpi(is / 4, 0, k, STEPS);
    tft.fillRoundRect(x, y, w, h, r, PAGE_BG);
  }
}

void fxCloseApp(){
  // El wallpaper reaparece desde los bordes hacia el centro: es el gesto
  // inverso a fxOpenApp y cuesta cuatro rectangulos por paso.
  const uint8_t STEPS = 5;
  for(uint8_t k = 1; k <= STEPS; k++){
    int16_t w = (int16_t)((int32_t)SCR_W * k / (2 * STEPS));
    int16_t h = (int16_t)((int32_t)SCR_H * k / (2 * STEPS));
    gfxWallRect(0, 0, w, SCR_H);
    gfxWallRect(SCR_W - w, 0, w, SCR_H);
    gfxWallRect(0, 0, SCR_W, h);
    gfxWallRect(0, SCR_H - h, SCR_W, h);
  }
  gfxWallpaper();
}
