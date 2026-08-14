// ###########################################################################
// Flex OS Super Single 1.0
// Arduino Uno (ATmega328P) + TFT LCD Shield 3.5" MAR3501 / ILI9486
//
// Edicion derivada de Flex OS UltraSingle para 32 KB Flash y 2 KB SRAM.
// Conserva la geometria, paleta, pantalla de bloqueo, escritorio, iconos,
// dock y panel rapido. No usa animaciones, framebuffer, String, malloc ni
// redibujado completo dentro del bucle normal: cada evento invalida solo su
// region. Los pintados completos ocurren exclusivamente al cambiar de vista
// y al variar el nivel de la app Linterna.
// ###########################################################################

#include <MCUFRIEND_kbv.h>
#include <TouchScreen.h>
#include <EEPROM.h>
#include <avr/pgmspace.h>

#if !defined(__AVR_ATmega328P__)
#error "Flex OS Super Single requiere Arduino Uno / ATmega328P"
#endif

// ---------------- Hardware: identico a la rama UltraSingle ----------------
#define SCR_W 320
#define SCR_H 480
#define TS_XP 8
#define TS_XM A2
#define TS_YP A3
#define TS_YM 9
#define TS_LEFT 76
#define TS_RT 905
#define TS_TOP 951
#define TS_BOT 68
#define TS_RXPLATE 300
#define TS_PRESS_MIN 60
#define TS_PRESS_MAX 1000

#define BAR_TOP_H 34
#define BAR_NAV_H 38
#define APP_HDR_H 40
#define RGB(r,g,b) ((uint16_t)((((r)&0xF8)<<8)|(((g)&0xFC)<<3)|((b)>>3)))

#define WALL_GREEN  RGB(80,224,74)
#define WALL_BLUE   RGB(40,150,245)
#define WALL_PURPLE RGB(112,46,230)
#define C_WHITE     RGB(255,255,255)
#define C_BLACK     RGB(0,0,0)
#define C_ACCENT    RGB(60,130,246)
#define C_ACCENT_DK RGB(28,58,120)
#define C_OK        RGB(80,200,120)
#define C_WARN      RGB(240,170,60)
#define C_DANGER    RGB(230,80,80)
#define C_ON_WALL   C_WHITE
#define C_ON_WALL_LO RGB(215,224,240)
#define GLASS_CLOCK RGB(40,62,128)
#define GLASS_CARD  RGB(40,50,90)
#define GLASS_DOCK  RGB(180,186,206)

static bool gDark = true;
static bool gGlass = true;
static bool gH24 = false;
#define PAGE_BG (gDark ? RGB(18,20,28) : RGB(244,247,251))
#define CARD_BG (gDark ? RGB(34,38,50) : RGB(255,255,255))
#define CARD_ALT (gDark ? RGB(48,54,72) : RGB(238,242,248))
#define TXT_HI (gDark ? RGB(240,242,248) : RGB(20,22,30))
#define TXT_LO (gDark ? RGB(160,166,182) : RGB(120,126,140))
#define BORDER (gDark ? RGB(66,74,94) : RGB(202,210,224))

static MCUFRIEND_kbv tft;
static TouchScreen ts(TS_XP, TS_YP, TS_XM, TS_YM, TS_RXPLATE);

enum AppId {
  APP_CLOCK, APP_CALENDAR, APP_CALC, APP_PAINT, APP_TIMER, APP_LIGHT,
  APP_WELL, APP_MEMORY, APP_SYSTEM, APP_SETTINGS, APP_LOCK, APP_ABOUT,
  APP_COUNT
};
enum ScreenId { ST_SPLASH, ST_LOCK, ST_HOME, ST_APP };

static uint8_t gScreen = ST_SPLASH;
static uint8_t gApp = APP_CLOCK;
static bool quickOpen = false;
static bool minuteDirty = false;
static uint32_t splashMs;
static uint32_t touches = 0;

// ---------------- Utilidades sin asignacion dinamica ----------------
static uint16_t mix565(uint16_t a, uint16_t b, uint8_t t) {
  uint8_t ar=(a>>11)&31, ag=(a>>5)&63, ab=a&31;
  uint8_t br=(b>>11)&31, bg=(b>>5)&63, bb=b&31;
  return (uint16_t)(((ar+((int16_t)(br-ar)*t)/255)<<11) |
                    ((ag+((int16_t)(bg-ag)*t)/255)<<5) |
                    (ab+((int16_t)(bb-ab)*t)/255));
}

static char* fmtU32(char* dst, uint32_t v) {
  char tmp[11]; uint8_t n=0;
  do { tmp[n++]=(char)('0'+v%10); v/=10; } while(v);
  while(n) *dst++=tmp[--n];
  *dst=0; return dst;
}

static void fmtI32(char* dst, int32_t v) {
  if(v<0) { *dst++='-'; v=-v; }
  fmtU32(dst,(uint32_t)v);
}

static char* fmt2(char* p, uint8_t v) {
  *p++=(char)('0'+(v/10)%10); *p++=(char)('0'+v%10); *p=0; return p;
}

static uint16_t freeRam() {
  extern int __heap_start;
  extern void* __brkval;
  int stack;
  return (uint16_t)((int)&stack - (__brkval ? (int)__brkval : (int)&__heap_start));
}

static bool hit(int16_t px,int16_t py,int16_t x,int16_t y,int16_t w,int16_t h) {
  return px>=x && px<x+w && py>=y && py<y+h;
}

// ---------------- Texto: todos los literales viven en Flash ----------------
static void textP(int16_t x,int16_t y,PGM_P s,uint8_t z,uint16_t c) {
  tft.setTextWrap(false); tft.setTextSize(z); tft.setTextColor(c); tft.setCursor(x,y);
  char ch; while((ch=(char)pgm_read_byte(s++))) tft.write(ch);
}

static int16_t textWP(PGM_P s,uint8_t z) { return (int16_t)strlen_P(s)*6*z; }
static void textCP(int16_t cx,int16_t y,PGM_P s,uint8_t z,uint16_t c) {
  textP(cx-textWP(s,z)/2,y,s,z,c);
}

static void textR(int16_t rx,int16_t y,const char* s,uint8_t z,uint16_t c) {
  tft.setTextWrap(false); tft.setTextSize(z); tft.setTextColor(c);
  tft.setCursor(rx-(int16_t)strlen(s)*6*z,y); while(*s)tft.write(*s++);
}

static void textC(int16_t cx,int16_t y,const char* s,uint8_t z,uint16_t c) {
  tft.setTextWrap(false); tft.setTextSize(z); tft.setTextColor(c);
  tft.setCursor(cx-(int16_t)strlen(s)*3*z,y); while(*s)tft.write(*s++);
}

static const char APP_NAMES[] PROGMEM =
  "Reloj\0Calendario\0Calculadora\0Paint\0Cron\xA2metro\0Linterna\0"
  "Bienestar\0Memoria\0Sistema\0Ajustes\0Bloqueo\0Acerca de";

static PGM_P nthP(PGM_P p,uint8_t n) {
  while(n--) { while(pgm_read_byte(p)) p++; p++; }
  return p;
}
static PGM_P appName(uint8_t id) { return nthP(APP_NAMES,id<APP_COUNT?id:0); }

// ---------------- Reloj interno ----------------
static uint8_t tmH=13, tmM=23, tmS=0, tmDay=4, tmMon=6;
static uint16_t tmYear=2026;
static uint32_t timeBase=0, bootBase=0;
static const uint8_t MDAYS[12] PROGMEM={31,28,31,30,31,30,31,31,30,31,30,31};
static const char MON_S[] PROGMEM="Ene\0Feb\0Mar\0Abr\0May\0Jun\0Jul\0Ago\0Sep\0Oct\0Nov\0Dic";
static const char MON_L[] PROGMEM="Enero\0Febrero\0Marzo\0Abril\0Mayo\0Junio\0Julio\0Agosto\0Septiembre\0Octubre\0Noviembre\0Diciembre";
static const char DAY_L[] PROGMEM="Domingo\0Lunes\0Martes\0Mi\x82rcoles\0Jueves\0Viernes\0S\xA0" "bado";

static bool leap(uint16_t y) { return (y%4==0 && y%100!=0) || y%400==0; }
static uint8_t dim(uint16_t y,uint8_t m) {
  uint8_t d=pgm_read_byte(&MDAYS[m%12]); return (m==1 && leap(y))?29:d;
}
static uint8_t dow(uint16_t y,uint8_t m,uint8_t d) {
  static const uint8_t T[12] PROGMEM={0,3,2,5,0,3,5,1,4,6,2,4};
  if(m<2) y--;
  return (uint8_t)((y+y/4-y/100+y/400+pgm_read_byte(&T[m])+d)%7);
}

static void clockText(char* b) {
  char* p=b; uint8_t h=tmH;
  if(!gH24) { h%=12; if(!h) h=12; p=fmtU32(p,h); }
  else p=fmt2(p,h);
  *p++=':'; fmt2(p,tmM);
}

static char* appendP(char* d,PGM_P s) {
  char c; while((c=(char)pgm_read_byte(s++))) *d++=c; *d=0; return d;
}

static void dateText(char* b,bool longName) {
  char* p=appendP(b,nthP(DAY_L,dow(tmYear,tmMon,tmDay)));
  *p++=','; *p++=' '; p=fmtU32(p,tmDay); *p++=' '; *p++='d'; *p++='e'; *p++=' ';
  appendP(p,nthP(longName?MON_L:MON_S,tmMon));
}

static void timeTick() {
  uint32_t now=millis(), elapsed=now-timeBase;
  if(elapsed<1000UL) return;
  uint16_t seconds=(uint16_t)(elapsed/1000UL); timeBase+=(uint32_t)seconds*1000UL;
  while(seconds--) {
    if(++tmS<60) continue;
    tmS=0; minuteDirty=true;
    if(++tmM<60) continue;
    tmM=0;
    if(++tmH<24) continue;
    tmH=0;
    if(++tmDay<=dim(tmYear,tmMon)) continue;
    tmDay=1; if(++tmMon<12) continue; tmMon=0; tmYear++;
  }
}

// ---------------- Persistencia minima: 8 bytes de EEPROM ----------------
static void saveFlags() {
  uint8_t f=(gDark?1:0)|(gGlass?2:0)|(gH24?4:0);
  EEPROM.update(3,f); EEPROM.update(4,tmH); EEPROM.update(5,tmM);
}

static void loadStore() {
  if(EEPROM.read(0)!='S' || EEPROM.read(1)!='S' || EEPROM.read(2)!=1) {
    EEPROM.update(0,'S'); EEPROM.update(1,'S'); EEPROM.update(2,1); saveFlags();
    return;
  }
  uint8_t f=EEPROM.read(3); gDark=f&1; gGlass=f&2; gH24=f&4;
  uint8_t h=EEPROM.read(4),m=EEPROM.read(5);
  if(h<24 && m<60) { tmH=h; tmM=m; }
}

// ---------------- Tactil: toque, arrastre y swipe, sin colas ----------------
struct TouchState {
  int16_t x,y,sx,sy,lx,ly;
  uint32_t downAt,lastAt;
  bool down,pressed,tap,swipeUp,swipeDown;
};
static TouchState tp;

static void pollTouch() {
  tp.pressed=tp.tap=tp.swipeUp=tp.swipeDown=false;
  TSPoint p=ts.getPoint();
  pinMode(TS_XM,OUTPUT); pinMode(TS_YP,OUTPUT);
  bool now=p.z>TS_PRESS_MIN && p.z<TS_PRESS_MAX;
  int16_t x=tp.x,y=tp.y;
  if(now) {
    long xx=map(p.x,TS_LEFT,TS_RT,0,SCR_W), yy=map(p.y,TS_TOP,TS_BOT,0,SCR_H);
    if(xx<0)xx=0; if(xx>=SCR_W)xx=SCR_W-1; if(yy<0)yy=0; if(yy>=SCR_H)yy=SCR_H-1;
    x=(int16_t)xx; y=(int16_t)yy;
  }
  if(now && !tp.down) {
    tp.down=true; tp.pressed=true; tp.sx=tp.lx=x; tp.sy=tp.ly=y;
    tp.downAt=millis(); touches++;
  } else if(!now && tp.down) {
    tp.down=false; int16_t dx=tp.x-tp.sx,dy=tp.y-tp.sy;
    int16_t ax=dx<0?-dx:dx,ay=dy<0?-dy:dy;
    if(ax<12 && ay<12 && millis()-tp.downAt<420UL) tp.tap=true;
    else if(ay>=45 && ay>ax) { tp.swipeUp=dy<0; tp.swipeDown=dy>0; }
  }
  if(now) { tp.lx=tp.x; tp.ly=tp.y; tp.x=x; tp.y=y; tp.lastAt=millis(); }
}

// ---------------- Motor grafico directo, cero framebuffer ----------------
static uint16_t wallAt(int16_t x,int16_t y) {
  uint16_t q=(uint16_t)(((uint32_t)y*192UL)/(SCR_H-1)+((uint32_t)(SCR_W-1-x)*63UL)/(SCR_W-1));
  return q<128?mix565(WALL_GREEN,WALL_BLUE,(uint8_t)(q*2)):
               mix565(WALL_BLUE,WALL_PURPLE,(uint8_t)((q-128)*2));
}

static void wallRect(int16_t x,int16_t y,int16_t w,int16_t h) {
  if(x<0){w+=x;x=0;} if(y<0){h+=y;y=0;}
  if(x+w>SCR_W)w=SCR_W-x; if(y+h>SCR_H)h=SCR_H-y; if(w<=0||h<=0)return;
  for(int16_t yy=y;yy<y+h;yy+=8) for(int16_t xx=x;xx<x+w;xx+=16) {
    int16_t bw=min((int16_t)16,(int16_t)(x+w-xx));
    int16_t bh=min((int16_t)8,(int16_t)(y+h-yy));
    tft.fillRect(xx,yy,bw,bh,wallAt(xx+bw/2,yy+bh/2));
  }
}

static void wallpaper() { wallRect(0,0,SCR_W,SCR_H); }

static void glassRect(int16_t x,int16_t y,int16_t w,int16_t h,uint8_t r,uint16_t tint,uint8_t a) {
  for(int16_t row=0;row<h;row++) {
    int16_t edge=min(row,(int16_t)(h-1-row));
    int16_t inset=edge<r?(r-edge):0;
    int16_t ww=w-inset*2; if(ww<=0)continue;
    uint16_t c=mix565(wallAt(x+w/2,y+row),tint,a);
    tft.drawFastHLine(x+inset,y+row,ww,c);
  }
}

static void card(int16_t x,int16_t y,int16_t w,int16_t h,uint8_t r=12) {
  tft.fillRoundRect(x,y,w,h,r,gGlass?mix565(PAGE_BG,CARD_ALT,150):CARD_BG);
}

static void thickLine(int16_t x0,int16_t y0,int16_t x1,int16_t y1,uint8_t n,uint16_t c) {
  for(int8_t k=-(int8_t)(n/2);k<=(int8_t)((n-1)/2);k++) tft.drawLine(x0,y0+k,x1,y1+k,c);
}

static const uint8_t SEG7[10] PROGMEM={0x3F,0x06,0x5B,0x4F,0x66,0x6D,0x7D,0x07,0x7F,0x6F};
static void segDigit(int16_t x,int16_t y,uint8_t w,uint8_t tk,uint8_t m,uint16_t c) {
  int16_t hh=w,dh=w*2,bw=w-tk,bh=hh-tk,rr=tk/2;
  if(m&1)tft.fillRoundRect(x+rr,y,bw,tk,rr,c);
  if(m&64)tft.fillRoundRect(x+rr,y+hh-rr,bw,tk,rr,c);
  if(m&8)tft.fillRoundRect(x+rr,y+dh-tk,bw,tk,rr,c);
  if(m&32)tft.fillRoundRect(x,y+rr,tk,bh,rr,c);
  if(m&2)tft.fillRoundRect(x+w-tk,y+rr,tk,bh,rr,c);
  if(m&16)tft.fillRoundRect(x,y+hh,tk,bh,rr,c);
  if(m&4)tft.fillRoundRect(x+w-tk,y+hh,tk,bh,rr,c);
}

static int16_t bigClockW(const char* s,uint8_t w) {
  int16_t n=0; while(*s) { n+=(*s++==':')?w/2+4:w+6; } return n? n-6:0;
}

static void bigClock(const char* s,int16_t cx,int16_t cy,uint8_t w,uint8_t tk,uint16_t c) {
  int16_t x=cx-bigClockW(s,w)/2,y=cy-w;
  while(*s) {
    if(*s==':') { tft.fillCircle(x+w/4,y+w*2/3,tk/2,c); tft.fillCircle(x+w/4,y+w*4/3,tk/2,c); x+=w/2+4; }
    else { segDigit(x,y,w,tk,pgm_read_byte(&SEG7[*s-'0']),c); x+=w+6; }
    s++;
  }
}

// ---------------- Cromo comun ----------------
static void drawBattery(int16_t x,int16_t y,uint16_t c) {
  tft.drawRoundRect(x,y,26,13,2,c); tft.fillRect(x+26,y+4,2,5,c); tft.fillRect(x+2,y+2,18,9,c);
}

static void drawSignal(int16_t x,int16_t y,uint16_t c) {
  uint16_t fr=freeRam(); uint8_t lit=fr>900?3:(fr>450?2:1);
  for(uint8_t i=0;i<3;i++) { uint8_t h=4+i*4; tft.fillRect(x+i*4,y-h,3,h,i<lit?c:mix565(c,C_BLACK,150)); }
}

static void statusClock() {
  wallRect(0,0,160,BAR_TOP_H); char b[32]; clockText(b);
  textC(46,4,b,2,C_ON_WALL); dateText(b,false); textC(72,21,b,1,C_ON_WALL_LO);
}

static void statusBar() {
  wallRect(0,0,SCR_W,BAR_TOP_H); statusClock(); drawSignal(258,22,C_ON_WALL); drawBattery(280,10,C_ON_WALL);
}

static void navBar(bool wall) {
  int16_t y=SCR_H-BAR_NAV_H;
  if(wall) wallRect(0,y,SCR_W,BAR_NAV_H); else tft.fillRect(0,y,SCR_W,BAR_NAV_H,PAGE_BG);
  uint16_t c=wall?C_ON_WALL:TXT_LO;
  tft.fillTriangle(45,y+17,59,y+9,59,y+25,c);
  tft.drawCircle(160,y+17,9,c); tft.drawCircle(160,y+17,8,c);
  tft.drawRoundRect(258,y+8,18,18,3,c);
}

static void appBase(PGM_P title) {
  tft.fillRect(0,0,SCR_W,SCR_H,PAGE_BG);
  thickLine(24,11,15,20,2,TXT_HI); thickLine(15,20,24,29,2,TXT_HI); tft.fillRect(15,19,16,2,TXT_HI);
  textCP(160,12,title,2,TXT_HI); tft.drawFastHLine(0,39,SCR_W,BORDER); navBar(false);
}

static void toggleAt(int16_t x,int16_t y,bool on) {
  tft.fillRoundRect(x,y,42,22,11,on?C_ACCENT:(gDark?RGB(70,76,92):RGB(200,206,218)));
  tft.fillCircle(on?x+31:x+11,y+11,8,C_WHITE);
}

static void rowToggle(int16_t y,PGM_P label,bool on) {
  card(12,y,296,50,10); textP(26,y+17,label,2,TXT_HI); toggleAt(246,y+14,on);
}

static void rowValue(int16_t y,PGM_P label,const char* value,PGM_P valueP=0) {
  card(12,y,296,50,10); textP(26,y+17,label,2,TXT_HI);
  if(valueP)textP(288-textWP(valueP,1),y+20,valueP,1,TXT_LO); else textR(288,y+20,value,1,TXT_LO);
}

// ---------------- Iconos vectoriales ----------------
static int16_t pc(uint8_t s,uint8_t p) { return (int16_t)((uint16_t)s*p/100); }
static void iconBase(int16_t x,int16_t y,uint8_t s,uint16_t bg) {
  uint8_t r=pc(s,24); tft.fillRoundRect(x,y,s,s,r,bg);
  tft.fillRoundRect(x,y,s,s/2,r,mix565(bg,C_WHITE,22));
}

static void ring(int16_t x,int16_t y,int16_t r,uint8_t n,uint16_t c) {
  while(n--) tft.drawCircle(x,y,r-n,c);
}

static void appIcon(uint8_t id,int16_t x,int16_t y,uint8_t s) {
  int16_t cx=x+s/2,cy=y+s/2; uint16_t w=C_WHITE; uint8_t tk=max((uint8_t)2,(uint8_t)(s/12));
  switch(id) {
    case APP_CLOCK:
      iconBase(x,y,s,RGB(245,245,247)); ring(cx,cy,pc(s,36),2,RGB(70,70,74));
      thickLine(cx,cy,cx-pc(s,14),cy-pc(s,10),2,RGB(30,30,30));
      thickLine(cx,cy,cx+pc(s,12),cy-pc(s,20),2,RGB(245,140,30)); tft.fillCircle(cx,cy,2,RGB(30,30,30)); break;
    case APP_CALENDAR: {
      iconBase(x,y,s,w); tft.fillRect(x+pc(s,16),y+pc(s,18),pc(s,68),pc(s,16),RGB(232,70,70));
      char b[3]; fmtU32(b,tmDay); textC(cx,y+pc(s,42),b,s>=44?3:2,RGB(60,60,64)); } break;
    case APP_CALC:
      iconBase(x,y,s,RGB(58,58,60)); tft.fillRoundRect(x+pc(s,18),y+pc(s,16),pc(s,64),pc(s,16),3,RGB(210,210,215));
      for(uint8_t r=0;r<3;r++)for(uint8_t c=0;c<4;c++)tft.fillRoundRect(x+pc(s,18)+c*pc(s,17),y+pc(s,40)+r*pc(s,15),pc(s,12),pc(s,10),2,c==3?RGB(245,150,30):RGB(150,150,155)); break;
    case APP_PAINT:
      iconBase(x,y,s,RGB(241,231,210)); tft.fillCircle(cx-2,cy+1,pc(s,27),RGB(236,226,205));
      tft.fillCircle(cx-8,cy-4,pc(s,5),C_DANGER); tft.fillCircle(cx+5,cy-7,pc(s,5),RGB(240,200,60));
      tft.fillCircle(cx+9,cy+3,pc(s,5),C_ACCENT); thickLine(cx+2,cy-8,cx+pc(s,26),cy-pc(s,30),2,RGB(140,100,60)); break;
    case APP_TIMER:
      iconBase(x,y,s,RGB(40,44,56)); tft.fillRect(cx-pc(s,8),y+pc(s,12),pc(s,16),pc(s,7),RGB(230,235,245));
      ring(cx,cy+2,pc(s,32),3,RGB(230,235,245)); thickLine(cx,cy+2,cx+pc(s,16),cy-pc(s,12),2,RGB(245,150,30)); break;
    case APP_LIGHT:
      iconBase(x,y,s,RGB(70,76,92)); tft.fillRoundRect(cx-pc(s,10),y+pc(s,20),pc(s,20),pc(s,14),3,RGB(232,236,246));
      tft.fillRoundRect(cx-pc(s,7),y+pc(s,32),pc(s,14),pc(s,34),3,RGB(180,188,205));
      tft.fillTriangle(cx-pc(s,22),y+pc(s,16),cx+pc(s,22),y+pc(s,16),cx,y+pc(s,2),RGB(250,220,110)); break;
    case APP_WELL:
      iconBase(x,y,s,RGB(92,193,90)); thickLine(cx-pc(s,16),cy,cx-1,cy+pc(s,16),tk,w); thickLine(cx-1,cy+pc(s,16),cx+pc(s,20),cy-pc(s,14),tk,w); break;
    case APP_MEMORY:
      iconBase(x,y,s,RGB(59,123,217)); tft.fillRoundRect(x+pc(s,18),y+pc(s,34),pc(s,64),pc(s,36),4,RGB(225,236,250));
      tft.fillRect(x+pc(s,26),y+pc(s,46),pc(s,48),3,RGB(140,175,225)); tft.fillRect(x+pc(s,26),y+pc(s,55),pc(s,32),3,RGB(140,175,225)); break;
    case APP_SYSTEM: case APP_ABOUT:
      iconBase(x,y,s,RGB(48,54,72)); tft.fillRoundRect(x+pc(s,26),y+pc(s,26),pc(s,48),pc(s,48),4,RGB(120,200,240));
      tft.fillRoundRect(x+pc(s,36),y+pc(s,36),pc(s,28),pc(s,28),2,RGB(30,36,50)); break;
    case APP_LOCK:
      iconBase(x,y,s,RGB(45,90,160)); tft.fillRoundRect(cx-pc(s,20),cy-pc(s,4),pc(s,40),pc(s,34),5,w);
      ring(cx,cy-pc(s,7),pc(s,14),3,w); tft.fillCircle(cx,cy+pc(s,10),3,C_ACCENT_DK); break;
    default:
      iconBase(x,y,s,RGB(138,143,152)); tft.fillCircle(cx,cy,pc(s,27),RGB(70,74,84));
      ring(cx,cy,pc(s,12),4,RGB(138,143,152)); break;
  }
}

// ---------------- Escritorio y panel rapido ----------------
#define WG_Y 40
#define WG_H 76
#define GR_Y0 128
#define GR_S 52
#define GR_X0 20
#define GR_CSTEP 76
#define GR_RSTEP 76
#define DK_X 20
#define DK_Y 372
#define DK_W 280
#define DK_H 62
#define DK_S 44
static const uint8_t DOCK[4] PROGMEM={APP_SETTINGS,APP_CALC,APP_PAINT,APP_CLOCK};
static const char S_MEMORY[] PROGMEM="Memoria";
static const char S_FREE[] PROGMEM="B libres";
static const char S_UPTIME[] PROGMEM="Encendido";
static const char S_MINUTES[] PROGMEM="minutos";
static const char S_QUICK[] PROGMEM="Panel r\xA0pido";
static const char S_DARK[] PROGMEM="Modo oscuro";
static const char S_GLASS[] PROGMEM="Vidrio";
static const char S_LOCK[] PROGMEM="Bloquear";
static const char S_UNLOCK[] PROGMEM="Desliza para desbloquear";

static void widgetMemory() {
  if(gGlass)glassRect(14,WG_Y,142,WG_H,14,C_ACCENT_DK,150); else tft.fillRoundRect(14,WG_Y,142,WG_H,14,C_ACCENT_DK);
  textP(26,WG_Y+10,S_MEMORY,1,C_ON_WALL); char b[8]; fmtU32(b,freeRam()); textC(58,WG_Y+26,b,2,C_ON_WALL);
  textP(26,WG_Y+50,S_FREE,1,C_ON_WALL_LO); uint8_t pctFree=(uint8_t)min(100UL,(uint32_t)freeRam()*100UL/2048UL);
  tft.fillRoundRect(26,WG_Y+62,118,5,2,RGB(20,30,60)); tft.fillRoundRect(26,WG_Y+62,(uint16_t)118*pctFree/100,5,2,C_OK);
}

static void widgetUptime() {
  if(gGlass)glassRect(164,WG_Y,142,WG_H,14,GLASS_CARD,120); else tft.fillRoundRect(164,WG_Y,142,WG_H,14,RGB(46,52,70));
  textP(176,WG_Y+10,S_UPTIME,1,C_ON_WALL); char b[10]; fmtU32(b,(millis()-bootBase)/60000UL);
  textC(202,WG_Y+28,b,2,C_ON_WALL); textP(224,WG_Y+34,S_MINUTES,1,C_ON_WALL_LO);
}

static void slotXY(uint8_t i,int16_t &x,int16_t &y) { x=GR_X0+(i%4)*GR_CSTEP; y=GR_Y0+(i/4)*GR_RSTEP; }
static void drawSlot(uint8_t i) {
  int16_t x,y; slotXY(i,x,y); wallRect(x-4,y-4,GR_S+8,GR_S+24); appIcon(i,x,y,GR_S);
  textCP(x+GR_S/2,y+GR_S+5,appName(i),1,C_ON_WALL);
}

static void dock() {
  if(gGlass)glassRect(DK_X,DK_Y,DK_W,DK_H,20,GLASS_DOCK,90); else tft.fillRoundRect(DK_X,DK_Y,DK_W,DK_H,20,RGB(40,46,64));
  for(uint8_t i=0;i<4;i++) appIcon(pgm_read_byte(&DOCK[i]),DK_X+12+i*70,DK_Y+9,DK_S);
}

static void homeDraw() {
  quickOpen=false; wallpaper(); statusBar(); widgetMemory(); widgetUptime();
  for(uint8_t i=0;i<APP_COUNT;i++) drawSlot(i);
  tft.fillCircle(160,362,3,C_WHITE); dock(); navBar(true);
}

static void quickDraw() {
  glassRect(8,8,304,210,22,GLASS_CLOCK,190); textP(28,24,S_QUICK,2,C_ON_WALL);
  textP(28,64,S_DARK,1,C_ON_WALL); toggleAt(240,56,gDark);
  textP(28,98,S_GLASS,1,C_ON_WALL); toggleAt(240,90,gGlass);
  textP(28,132,appName(APP_LIGHT),1,C_ON_WALL); textP(28,166,S_LOCK,1,C_ON_WALL);
  tft.drawLine(274,135,282,142,C_ON_WALL); tft.drawLine(282,142,274,149,C_ON_WALL);
  tft.drawLine(274,169,282,176,C_ON_WALL); tft.drawLine(282,176,274,183,C_ON_WALL);
}

static void quickClose() {
  quickOpen=false; wallRect(0,0,SCR_W,226); statusBar(); widgetMemory(); widgetUptime();
  for(uint8_t i=0;i<8;i++) drawSlot(i); dock();
}

static int8_t homeHitApp(int16_t x,int16_t y) {
  for(uint8_t i=0;i<APP_COUNT;i++) { int16_t sx,sy; slotXY(i,sx,sy); if(hit(x,y,sx-5,sy-4,GR_S+10,GR_S+22))return i; }
  for(uint8_t i=0;i<4;i++) if(hit(x,y,DK_X+8+i*70,DK_Y+5,54,54))return pgm_read_byte(&DOCK[i]);
  return -1;
}

// ---------------- Pantalla de bloqueo ----------------
static const char S_UNO[] PROGMEM="Arduino Uno";
static const char S_RAMFREE[] PROGMEM="SRAM disponible";
static const char S_MCU_VALUE[] PROGMEM="ATmega328P - 16 MHz";

static void lockClock() {
  if(gGlass)glassRect(18,110,284,150,24,GLASS_CLOCK,150); else wallRect(18,110,284,150);
  char b[40]; clockText(b); bigClock(b,160,172,40,9,C_ON_WALL); dateText(b,true); textC(160,228,b,1,C_ON_WALL_LO);
}

static void lockCard(int16_t y,PGM_P title,const char* value,uint16_t accent,PGM_P valueP=0) {
  if(gGlass)glassRect(18,y,284,48,14,GLASS_CARD,130); else tft.fillRoundRect(18,y,284,48,14,RGB(40,46,64));
  tft.fillCircle(44,y+24,9,accent); textP(64,y+10,title,1,C_ON_WALL);
  if(valueP)textCP(160,y+27,valueP,1,C_ON_WALL_LO); else textC(160,y+27,value,1,C_ON_WALL_LO);
}

static void lockDraw() {
  wallpaper(); drawSignal(258,26,C_ON_WALL); drawBattery(280,14,C_ON_WALL); lockClock();
  char b[16]; fmtU32(b,freeRam()); char* p=b+strlen(b); *p++=' ';*p++='B';*p=0;
  lockCard(276,S_RAMFREE,b,C_OK); lockCard(332,S_UNO,0,C_WARN,S_MCU_VALUE);
  tft.fillRoundRect(114,388,92,7,3,C_WHITE); textCP(160,410,S_UNLOCK,1,C_ON_WALL);
}

// ---------------- Apps ----------------
static void goHome();
static void openApp(uint8_t id);

// Reloj
static void clockAppFace() {
  tft.fillRect(20,64,280,330,PAGE_BG); card(28,74,264,300,22);
  int16_t cx=160,cy=205; ring(cx,cy,92,3,TXT_HI);
  tft.fillRect(cx-2,cy-82,4,12,TXT_LO); tft.fillRect(cx-2,cy+70,4,12,TXT_LO);
  tft.fillRect(cx-82,cy-2,12,4,TXT_LO); tft.fillRect(cx+70,cy-2,12,4,TXT_LO);
  static const int8_t V[12][2] PROGMEM={{0,-60},{30,-52},{52,-30},{60,0},{52,30},{30,52},{0,60},{-30,52},{-52,30},{-60,0},{-52,-30},{-30,-52}};
  uint8_t mi=tmM/5,hi=(tmH%12);
  int8_t mx=pgm_read_byte(&V[mi][0]),my=pgm_read_byte(&V[mi][1]);
  int8_t hx=pgm_read_byte(&V[hi][0]),hy=pgm_read_byte(&V[hi][1]);
  thickLine(cx,cy,cx+mx,cy+my,3,C_ACCENT); thickLine(cx,cy,cx+hx*2/3,cy+hy*2/3,5,TXT_HI); tft.fillCircle(cx,cy,5,C_ACCENT);
  char b[8]; clockText(b); textC(160,330,b,3,TXT_HI);
}

// Calendario
static uint8_t calMon; static uint16_t calYear;
static const char DAYS[] PROGMEM="D\0L\0M\0X\0J\0V\0S";
static void calendarBody() {
  tft.fillRect(8,52,304,378,PAGE_BG); card(12,58,296,360,16);
  textCP(160,72,nthP(MON_L,calMon),2,TXT_HI); char y[6]; fmtU32(y,calYear); textC(160,94,y,1,TXT_LO);
  tft.fillTriangle(34,85,48,76,48,94,C_ACCENT); tft.fillTriangle(286,85,272,76,272,94,C_ACCENT);
  for(uint8_t i=0;i<7;i++) textCP(34+i*42,125,nthP(DAYS,i),1,TXT_LO);
  uint8_t first=dow(calYear,calMon,1),days=dim(calYear,calMon);
  for(uint8_t d=1;d<=days;d++) {
    uint8_t q=first+d-1,col=q%7,row=q/7; int16_t x=34+col*42,y0=153+row*42;
    bool today=d==tmDay && calMon==tmMon && calYear==tmYear;
    if(today)tft.fillCircle(x,y0+4,15,C_ACCENT);
    char b[3]; fmtU32(b,d); textC(x,y0,b,1,today?C_WHITE:TXT_HI);
  }
}

// Calculadora entera
static int32_t calcValue=0,calcSaved=0; static char calcOp=0; static bool calcFresh=true,calcErr=false;
static const char CALC_KEYS[16] PROGMEM={'7','8','9','/','4','5','6','*','1','2','3','-','C','0','=','+'};
static void calcDisplay() {
  tft.fillRoundRect(18,56,284,66,12,CARD_BG); char b[14];
  if(calcErr) { strcpy_P(b,PSTR("Error")); } else fmtI32(b,calcValue);
  textR(286,78,b,3,TXT_HI);
}

static void calcKeys() {
  for(uint8_t i=0;i<16;i++) { int16_t x=18+(i%4)*72,y=138+(i/4)*65; char k=pgm_read_byte(&CALC_KEYS[i]);
    tft.fillRoundRect(x,y,62,54,12,(i%4==3||k=='=')?C_ACCENT:CARD_ALT); char b[2]={k,0}; textC(x+31,y+16,b,2,(i%4==3||k=='=')?C_WHITE:TXT_HI); }
}

static void calcApply() {
  if(!calcOp)return; int32_t a=calcSaved,b=calcValue;
  if(calcOp=='+')calcValue=a+b; else if(calcOp=='-')calcValue=a-b; else if(calcOp=='*')calcValue=a*b;
  else if(!b)calcErr=true; else calcValue=a/b;
  calcOp=0;
}

static void calcTap(int16_t x,int16_t y) {
  if(!hit(x,y,18,138,278,249))return; uint8_t c=(x-18)/72,r=(y-138)/65; if(c>3||r>3)return;
  char k=pgm_read_byte(&CALC_KEYS[r*4+c]);
  if(k=='C'){calcValue=calcSaved=0;calcOp=0;calcFresh=true;calcErr=false;}
  else if(k>='0'&&k<='9'){if(calcFresh||calcErr){calcValue=0;calcFresh=false;calcErr=false;} if(calcValue<1000000L)calcValue=calcValue*10+k-'0';}
  else if(k=='='){calcApply();calcFresh=true;} else {calcApply();calcSaved=calcValue;calcOp=k;calcFresh=true;}
  calcDisplay();
}

// Paint
static uint8_t paintColor=0;
static const uint16_t PALETTE[6] PROGMEM={C_BLACK,C_DANGER,C_WARN,C_OK,C_ACCENT,WALL_PURPLE};
static void paintBar() {
  tft.fillRect(0,396,SCR_W,46,CARD_BG);
  for(uint8_t i=0;i<6;i++){int16_t x=34+i*50;uint16_t c=pgm_read_word(&PALETTE[i]);tft.fillCircle(x,419,14,c);if(i==paintColor)ring(x,419,18,2,C_ACCENT);}
}

// Cronometro
static bool timerRun=false; static uint32_t timerAccum=0,timerStart=0,timerPaint=0;
static uint32_t timerValue(){return timerAccum+(timerRun?millis()-timerStart:0);}
static const char S_START[] PROGMEM="Iniciar"; static const char S_STOP[] PROGMEM="Parar"; static const char S_RESET[] PROGMEM="Reiniciar";
static void timerDigits() {
  tft.fillRect(18,104,284,134,PAGE_BG); card(24,112,272,118,18); uint32_t cs=timerValue()/10;
  uint8_t mm=(uint8_t)min(99UL,cs/6000UL),ss=(uint8_t)((cs/100)%60),cc=(uint8_t)(cs%100); char b[9]; char* p=fmt2(b,mm);*p++=':';p=fmt2(p,ss);*p++='.';fmt2(p,cc);textC(160,158,b,3,TXT_HI);
}
static void timerButtons() {
  tft.fillRect(18,270,284,70,PAGE_BG); tft.fillRoundRect(24,278,128,52,16,C_ACCENT); tft.fillRoundRect(168,278,128,52,16,CARD_ALT);
  textCP(88,295,timerRun?S_STOP:S_START,2,C_WHITE); textCP(232,295,S_RESET,2,TXT_HI);
}

// Linterna
static uint8_t lightLevel=0;
static const char S_LIGHT_HINT[] PROGMEM="Toca para cambiar - abajo: salir";
static void lightDraw() {
  static const uint8_t L[3] PROGMEM={255,170,90};uint8_t v=pgm_read_byte(&L[lightLevel]);uint16_t c=RGB(v,v,v);
  tft.fillRect(0,0,SCR_W,SCR_H,c); textCP(160,24,S_LIGHT_HINT,1,v<150?C_WHITE:C_BLACK); tft.fillRoundRect(115,445,90,5,2,v<150?C_WHITE:C_BLACK);
}

// Apps informativas y ajustes
static const char S_TOTAL_TOUCH[] PROGMEM="Toques totales";
static const char S_FLASH[] PROGMEM="Flash"; static const char S_EEPROM[] PROGMEM="EEPROM"; static const char S_BOARD[] PROGMEM="Placa";
static const char S_PANEL[] PROGMEM="Panel"; static const char S_VERSION[] PROGMEM="Versi\xA2n"; static const char S_H24[] PROGMEM="Formato 24 h";
static const char S_HOUR[] PROGMEM="Hora"; static const char S_MINUTE[] PROGMEM="Minuto";
static const char V_32K[] PROGMEM="32 KB"; static const char V_1K[] PROGMEM="1 KB";
static const char V_PANEL[] PROGMEM="ILI9486 320x480";
static const char V_MCU[] PROGMEM="ATmega328P"; static const char V_RELEASE[] PROGMEM="Super Single 1.0";
static const char V_MAR[] PROGMEM="MAR3501 / ILI9486"; static const char V_MINUS[] PROGMEM="-"; static const char V_PLUS[] PROGMEM="+";

static void infoApps(uint8_t id) {
  char b[18];
  if(id==APP_WELL) {
    rowValue(72,S_TOTAL_TOUCH,(fmtU32(b,touches),b)); fmtU32(b,(millis()-bootBase)/60000UL); rowValue(134,S_UPTIME,b);
  } else if(id==APP_MEMORY) {
    fmtU32(b,freeRam()); rowValue(72,S_MEMORY,b); rowValue(134,S_FLASH,0,V_32K); rowValue(196,S_EEPROM,0,V_1K);
  } else if(id==APP_SYSTEM) {
    rowValue(72,S_BOARD,0,S_UNO); rowValue(134,S_PANEL,0,V_PANEL); rowValue(196,S_FLASH,0,V_MCU);
  } else {
    rowValue(72,S_VERSION,0,V_RELEASE); rowValue(134,S_BOARD,0,S_UNO); rowValue(196,S_PANEL,0,V_MAR);
  }
}

static void adjustRow(int16_t y,PGM_P label,uint8_t v) {
  card(12,y,296,50,10); textP(26,y+17,label,2,TXT_HI); char b[4];fmtU32(b,v);textC(258,y+20,b,1,TXT_HI);
  textCP(222,y+15,V_MINUS,2,C_ACCENT);textCP(292,y+15,V_PLUS,2,C_ACCENT);
}

static void settingsDraw() {
  rowToggle(66,S_DARK,gDark); rowToggle(124,S_GLASS,gGlass); rowToggle(182,S_H24,gH24);
  adjustRow(240,S_HOUR,tmH); adjustRow(298,S_MINUTE,tmM);
}

static void openApp(uint8_t id) {
  if(id==APP_LOCK){gScreen=ST_LOCK;quickOpen=false;lockDraw();return;}
  gScreen=ST_APP;gApp=id;quickOpen=false;
  if(id==APP_LIGHT){lightDraw();return;}
  appBase(appName(id));
  switch(id) {
    case APP_CLOCK:clockAppFace();break;
    case APP_CALENDAR:calMon=tmMon;calYear=tmYear;calendarBody();break;
    case APP_CALC:calcValue=calcSaved=0;calcOp=0;calcFresh=true;calcErr=false;calcDisplay();calcKeys();break;
    case APP_PAINT:tft.fillRect(0,40,SCR_W,356,C_WHITE);paintBar();break;
    case APP_TIMER:timerRun=false;timerAccum=0;timerDigits();timerButtons();break;
    case APP_SETTINGS:settingsDraw();break;
    default:infoApps(id);break;
  }
}

static void goHome() { gScreen=ST_HOME; homeDraw(); }

static void homeTick() {
  if(minuteDirty&&!quickOpen){statusClock();widgetUptime();}
  if(quickOpen) {
    if(tp.swipeUp||(tp.tap&&tp.y>226)){quickClose();return;}
    if(!tp.tap)return;
    if(hit(tp.x,tp.y,20,50,280,34)){gDark=!gDark;saveFlags();toggleAt(240,56,gDark);}
    else if(hit(tp.x,tp.y,20,84,280,34)){gGlass=!gGlass;saveFlags();toggleAt(240,90,gGlass);}
    else if(hit(tp.x,tp.y,20,118,280,34))openApp(APP_LIGHT);
    else if(hit(tp.x,tp.y,20,152,280,34)){gScreen=ST_LOCK;lockDraw();}
    return;
  }
  if(tp.swipeDown&&tp.sy<60){quickOpen=true;quickDraw();return;}
  if(tp.tap){int8_t id=homeHitApp(tp.x,tp.y);if(id>=0)openApp((uint8_t)id);}
}

static void appTick() {
  if(gApp==APP_LIGHT) { if(tp.tap){if(tp.y>420)goHome();else{lightLevel=(lightLevel+1)%3;lightDraw();}} return; }
  if(tp.tap&&(tp.y>=SCR_H-BAR_NAV_H||(tp.y<APP_HDR_H&&tp.x<60))){goHome();return;}
  switch(gApp) {
    case APP_CLOCK:if(minuteDirty)clockAppFace();break;
    case APP_CALENDAR:
      if(tp.tap&&tp.y>55&&tp.y<116){if(tp.x<90){if(!calMon){calMon=11;calYear--;}else calMon--;}else if(tp.x>230){if(++calMon>11){calMon=0;calYear++;}}calendarBody();}break;
    case APP_CALC:if(tp.tap)calcTap(tp.x,tp.y);break;
    case APP_PAINT:
      if(tp.pressed&&tp.y>=396){for(uint8_t i=0;i<6;i++)if(hit(tp.x,tp.y,14+i*50,398,40,42)){paintColor=i;paintBar();break;}}
      else if(tp.down&&tp.y>42&&tp.y<394&&tp.ly>42&&tp.ly<394)thickLine(tp.lx,tp.ly,tp.x,tp.y,4,pgm_read_word(&PALETTE[paintColor]));break;
    case APP_TIMER:
      if(tp.tap&&hit(tp.x,tp.y,24,278,128,52)){if(timerRun){timerAccum+=millis()-timerStart;timerRun=false;}else{timerStart=millis();timerRun=true;}timerButtons();}
      else if(tp.tap&&hit(tp.x,tp.y,168,278,128,52)){timerRun=false;timerAccum=0;timerDigits();timerButtons();}
      if(timerRun&&millis()-timerPaint>=100UL){timerPaint=millis();timerDigits();}break;
    case APP_SETTINGS:
      if(tp.tap&&hit(tp.x,tp.y,12,66,296,50)){gDark=!gDark;saveFlags();toggleAt(246,80,gDark);}
      else if(tp.tap&&hit(tp.x,tp.y,12,124,296,50)){gGlass=!gGlass;saveFlags();toggleAt(246,138,gGlass);}
      else if(tp.tap&&hit(tp.x,tp.y,12,182,296,50)){gH24=!gH24;saveFlags();toggleAt(246,196,gH24);}
      else if(tp.tap&&hit(tp.x,tp.y,198,240,110,50)){tmH=(uint8_t)((tmH+(tp.x>255?1:23))%24);saveFlags();tft.fillRect(198,240,110,50,PAGE_BG);adjustRow(240,S_HOUR,tmH);}
      else if(tp.tap&&hit(tp.x,tp.y,198,298,110,50)){tmM=(uint8_t)((tmM+(tp.x>255?1:59))%60);tmS=0;saveFlags();tft.fillRect(198,298,110,50,PAGE_BG);adjustRow(298,S_MINUTE,tmM);}break;
  }
}

// ---------------- Arranque y bucle ----------------
static const char S_FLEX[] PROGMEM="Flex OS";
static const char S_SUPER[] PROGMEM="Super Single";
static const char S_ARDUINO[] PROGMEM="Arduino Uno";

static void splashDraw() {
  tft.fillScreen(C_BLACK); for(uint8_t r=4;r<=34;r+=6)tft.drawCircle(160,180,r,mix565(C_BLACK,C_ACCENT,180));
  tft.fillCircle(160,180,12,C_ACCENT); textCP(160,250,S_FLEX,3,C_WHITE);textCP(160,282,S_SUPER,2,C_ACCENT);textCP(160,440,S_ARDUINO,1,RGB(110,116,132));
}

void setup() {
  // El hardware de esta edicion es fijo. Pasar el ID como constante permite
  // que LTO elimine del binario todos los controladores que no son ILI9486.
  tft.begin(0x9486);tft.setRotation(0);tft.cp437(true);
  loadStore();timeBase=bootBase=millis();memset(&tp,0,sizeof(tp));tp.lastAt=millis();
  splashDraw();splashMs=millis();
}

void loop() {
  pollTouch();timeTick();
  if(gScreen==ST_SPLASH){if(millis()-splashMs>=700UL){gScreen=ST_LOCK;lockDraw();}}
  else if(gScreen==ST_LOCK){if(minuteDirty)lockClock();if(tp.swipeUp||(tp.tap&&tp.y>370))goHome();}
  else if(gScreen==ST_HOME)homeTick();
  else appTick();
  if((gScreen==ST_HOME||gScreen==ST_APP)&&millis()-tp.lastAt>60000UL){gScreen=ST_LOCK;lockDraw();}
  minuteDirty=false;
  delay(18);
}
