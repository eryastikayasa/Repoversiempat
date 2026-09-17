#include "display_face.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "esp_attr.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

static EXT_RAM_BSS_ATTR uint8_t s_fb[DISPLAY_FACE_BUFFER_SIZE] = {0};
static face_state_t s_state = FACE_IDLE, s_prev = FACE_IDLE;
static uint32_t s_started = 0, s_override_until = 0;
static bool s_override = false;
static uint32_t s_rng = 0x6D2B79F5u, s_next_behavior = 0, s_next_blink = 0;
static int s_gx = 0, s_gy = 0, s_tx = 0, s_ty = 0;
static int s_mx = 0, s_my = 0, s_mtx = 0, s_mty = 0;
static uint8_t s_blink = 0, s_mouth = 0;
static uint32_t s_blink_start = 0, s_blink_dur = 70, s_mouth_until = 0, s_transition = 0;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t rnd(void) { uint32_t x=s_rng;x^=x<<13;x^=x>>17;x^=x<<5;s_rng=x?x:0xA341316Cu;return s_rng; }
static int rr(int a,int b) { return a>=b?a:a+(int)(rnd()%(uint32_t)(b-a+1)); }
static int clampi(int v,int a,int b) { return v<a?a:v>b?b:v; }
static int smoothi(int v,int t,int p) { if(v==t)return v;int d=(t-v)*p/100;return v+(d?d:(t>v?1:-1)); }
static void px(int x,int y,bool on=true) { if(x<0||x>=128||y<0||y>=64)return;uint8_t &b=s_fb[x+(y>>3)*128];uint8_t m=(uint8_t)(1u<<(y&7));if(on)b|=m;else b&=(uint8_t)~m; }
static void ln(int x0,int y0,int x1,int y1) { int dx=abs(x1-x0),sx=x0<x1?1:-1,dy=-abs(y1-y0),sy=y0<y1?1:-1,e=dx+dy;for(;;){px(x0,y0);if(x0==x1&&y0==y1)return;int e2=2*e;if(e2>=dy){e+=dy;x0+=sx;}if(e2<=dx){e+=dx;y0+=sy;}} }
static void circ(int cx,int cy,int r,bool on=true) { for(int y=-r;y<=r;y++){int q=r*r-y*y,dx=q>0?(int)sqrtf((float)q):0;for(int x=-dx;x<=dx;x++)px(cx+x,cy+y,on);} }
static void ears(int oy) { ln(18,14+oy,20,4+oy);ln(20,4+oy,30,13+oy);ln(98,13+oy,108,4+oy);ln(108,4+oy,110,14+oy);ln(21,11+oy,23,8+oy);ln(23,8+oy,27,12+oy);ln(101,12+oy,105,8+oy);ln(105,8+oy,107,11+oy); }
static void brow(int cx,int cy,int lift=0) { ln(cx-13,cy+2-lift,cx-7,cy-3-lift);ln(cx-7,cy-3-lift,cx,cy-5-lift);ln(cx,cy-5-lift,cx+7,cy-3-lift);ln(cx+7,cy-3-lift,cx+13,cy+2-lift); }
static void attentive(int cx,int cy) { ln(cx-13,cy+1,cx-6,cy-3);ln(cx-6,cy-3,cx,cy-4);ln(cx,cy-4,cx+6,cy-3);ln(cx+6,cy-3,cx+13,cy+1); }
static void thinking(int cx,int cy,bool left) { if(left)ln(cx-13,cy-1,cx+10,cy-5);else ln(cx-10,cy-5,cx+13,cy-1); }
static void sadbrow(int cx,int cy,bool left) { if(left){ln(cx-13,cy-1,cx,cy+5);ln(cx,cy+5,cx+13,cy+9);}else{ln(cx-13,cy+9,cx,cy+5);ln(cx,cy+5,cx+13,cy-1);} }
static void sadeye(int cx,int cy,int gy) { int y=cy+gy;ln(cx-11,y,cx-5,y+2);ln(cx-5,y+2,cx+5,y+2);ln(cx+5,y+2,cx+11,y); }
static void sleep_eye(int cx,int cy) { ln(cx-12,cy,cx+12,cy);ln(cx-8,cy+1,cx+8,cy+1); }
static void happy_eye(int cx,int cy) { for(int x=-12;x<=12;x++){float t=(float)x/12.f;int y=(int)(6.f*(1.f-t*t));px(cx+x,cy+y);if(!(x&1))px(cx+x,cy+y+1);} }
static void blink_eye(int cx,int cy,int open) { if(open<=0){ln(cx-11,cy,cx+11,cy);return;}int h=5+open/3;ln(cx-11,cy+1,cx-h,cy);ln(cx-h,cy,cx,cy+1);ln(cx,cy+1,cx+h,cy);ln(cx+h,cy,cx+11,cy+1); }
static void erroreye(int cx,int cy,int p) { int s=9+p;for(int i=-s;i<=s;i++){px(cx+i,cy+i);px(cx+i,cy-i);} }
static void mouth(int shape,int oy) { int cx=64,cy=53+oy;switch(shape){case 1:ln(cx-4,cy-1,cx,cy+1);ln(cx,cy+1,cx+4,cy-1);break;case 2:ln(cx-5,cy-2,cx,cy+3);ln(cx,cy+3,cx+5,cy-2);ln(cx-5,cy-2,cx+5,cy-2);break;case 3:ln(cx-7,cy-2,cx,cy+4);ln(cx,cy+4,cx+7,cy-2);ln(cx-7,cy-2,cx+7,cy-2);ln(cx-3,cy+1,cx+3,cy+1);break;default:ln(cx-5,cy,cx+5,cy);break;} }
static void behavior(uint32_t now,face_state_t st) { if((int32_t)(now-s_next_behavior)<0)return;if(st==FACE_SLEEP){s_tx=s_ty=s_mtx=s_mty=0;s_next_behavior=now+800;return;}if(st==FACE_IDLE){int r=rr(0,99);if(r<60)s_tx=s_ty=s_mtx=s_mty=0;else{s_tx=rr(-5,5);s_ty=rr(-3,2);s_mtx=rr(-1,1);s_mty=rr(-1,1);}s_next_behavior=now+(uint32_t)rr(450,1800);}else if(st==FACE_LISTENING){s_tx=rr(-2,2);s_ty=rr(-1,1);s_mtx=s_mty=0;s_next_behavior=now+(uint32_t)rr(700,1500);}else if(st==FACE_THINKING){int r=rr(0,2);s_tx=r==1?-3:r==2?3:0;s_ty=r?-3:-4;s_mtx=s_mty=0;s_next_behavior=now+(uint32_t)rr(800,1700);}else if(st==FACE_SPEAKING){s_tx=rr(-2,2);s_ty=rr(-1,1);s_mtx=rr(-1,1);s_mty=0;s_next_behavior=now+(uint32_t)rr(600,1300);}else{s_tx=s_ty=s_mtx=s_mty=0;s_next_behavior=now+1000;} }
static void update_blink(uint32_t now,face_state_t st) { if(st==FACE_SLEEP){s_blink=0;s_next_blink=now+1000;return;}if(!s_blink){if(!s_next_blink)s_next_blink=now+(uint32_t)rr(1800,5200);if((int32_t)(now-s_next_blink)>=0){s_blink=1;s_blink_start=now;s_blink_dur=(uint32_t)rr(55,105);}}else if(now-s_blink_start>=s_blink_dur){s_blink=0;s_next_blink=now+(uint32_t)rr(1800,5200);} }
static int blink_open(uint32_t now) { if(!s_blink)return 14;uint32_t t=now-s_blink_start,d=s_blink_dur/2?s_blink_dur/2:1;return t<d?14-(int)(t*14/d):(int)((t-d)*14/d); }
static void render_frame(uint32_t now) { memset(s_fb,0,sizeof(s_fb));face_state_t st;portENTER_CRITICAL(&s_mux);st=s_state;portEXIT_CRITICAL(&s_mux);behavior(now,st);s_gx=smoothi(s_gx,s_tx,18);s_gy=smoothi(s_gy,s_ty,16);s_mx=smoothi(s_mx,s_mtx,14);s_my=smoothi(s_my,s_mty,14);update_blink(now,st);if(st==FACE_SPEAKING&&(int32_t)(now-s_mouth_until)>=0){s_mouth=(uint8_t)rr(0,3);s_mouth_until=now+(uint32_t)rr(60,160);}if(st!=FACE_SPEAKING)s_mouth=0;uint32_t t=now-s_started;int ox=s_mx,oy=s_my,gx=s_gx,gy=s_gy,op=blink_open(now);bool happy=false,sad=false,sleep=false,err=false,curious=false;switch(st){case FACE_LISTENING:oy--;gx=clampi(gx,-3,3);gy=clampi(gy,-2,2);break;case FACE_THINKING:gy=clampi(gy,-5,-2);break;case FACE_SPEAKING:gx=clampi(gx,-3,3);gy=clampi(gy,-2,2);break;case FACE_HAPPY:happy=true;if(t<220)oy-=(int)((220-t)/110);break;case FACE_SAD:sad=true;gy=3;oy++;break;case FACE_ERROR:err=true;if(t<500)ox+=((t/140)&1)?1:-1;break;case FACE_SLEEP:sleep=true;gx=gy=ox=0;oy+=((t/1800)&1)?0:1;break;case FACE_IDLE:curious=(abs(gx)>1||abs(gy)>1);break;default:break;}if((int32_t)(now-s_transition)<220){int p=((now-s_transition)<110)?1:0;if(st==FACE_LISTENING)oy-=p;else if(st==FACE_SPEAKING)oy+=p;else if(st==FACE_ERROR)ox+=p;}ears(oy);int lx=34+ox,rx=94+ox,ey=28+oy;if(sleep){sleep_eye(lx,ey+1);sleep_eye(rx,ey+1);}else if(err){erroreye(lx,ey,(t<600)&&((t/260)&1));erroreye(rx,ey,(t<600)&&((t/260)&1));}else if(happy){happy_eye(lx,ey);happy_eye(rx,ey);}else if(sad){sadeye(lx,ey,gy);sadeye(rx,ey,gy);}else if(s_blink){blink_eye(lx,ey,op);blink_eye(rx,ey,op);}else{circ(lx,ey,14);circ(rx,ey,14);circ(lx+clampi(gx,-7,7),ey+clampi(gy,-5,5),7,false);circ(rx+clampi(gx,-7,7),ey+clampi(gy,-5,5),7,false);circ(lx+gx-2,ey+gy-3,2);circ(rx+gx-2,ey+gy-3,2);}int by=14+oy;switch(st){case FACE_LISTENING:attentive(lx,by);attentive(rx,by);break;case FACE_THINKING:thinking(lx,by,true);thinking(rx,by,false);break;case FACE_SAD:sadbrow(lx,by,true);sadbrow(rx,by,false);break;default:brow(lx,by,st==FACE_HAPPY?1:(curious?2:0));brow(rx,by,st==FACE_HAPPY?1:(curious?2:0));break;}if(st==FACE_HAPPY)mouth(2,oy);else if(st==FACE_SAD){ln(55,56+oy,64,53+oy);ln(64,53+oy,73,56+oy);}else if(st==FACE_ERROR){ln(57,55+oy,62,52+oy);ln(62,52+oy,67,55+oy);ln(67,55+oy,72,52+oy);}else mouth(st==FACE_SPEAKING?s_mouth:0,oy); }
void display_face_init(void){uint32_t n=(uint32_t)(esp_timer_get_time()/1000ULL);portENTER_CRITICAL(&s_mux);s_state=FACE_IDLE;s_prev=FACE_IDLE;s_started=n;s_override=false;s_override_until=0;portEXIT_CRITICAL(&s_mux);s_rng^=n+0x9E3779B9u;s_next_behavior=n+500;s_next_blink=n+(uint32_t)rr(1800,4200);s_blink=0;s_gx=s_gy=s_tx=s_ty=s_mx=s_my=s_mtx=s_mty=0;s_mouth=0;s_mouth_until=n;s_transition=n;render_frame(n);}
void display_face_update(uint32_t n){portENTER_CRITICAL(&s_mux);if(s_override&&(int32_t)(n-s_override_until)>=0){s_override=false;s_state=s_prev;s_started=n;s_next_behavior=n;}portEXIT_CRITICAL(&s_mux);render_frame(n);}
void display_face_set_state(face_state_t st){if(st<FACE_IDLE||st>FACE_SLEEP)st=FACE_IDLE;uint32_t n=(uint32_t)(esp_timer_get_time()/1000ULL);portENTER_CRITICAL(&s_mux);if(st==s_state){portEXIT_CRITICAL(&s_mux);return;}s_prev=s_state;s_state=st;s_started=n;s_transition=n;s_override=false;portEXIT_CRITICAL(&s_mux);s_next_behavior=n+350;s_mtx=s_mty=0;if(st==FACE_SPEAKING)s_mouth_until=n;}
void display_face_show_for_ms(face_state_t st,uint32_t ms){if(!ms){display_face_set_state(st);return;}uint32_t n=(uint32_t)(esp_timer_get_time()/1000ULL);portENTER_CRITICAL(&s_mux);s_prev=s_state;s_state=st;s_started=n;s_transition=n;s_override_until=n+ms;s_override=true;portEXIT_CRITICAL(&s_mux);s_next_behavior=n+350;s_mtx=s_mty=0;if(st==FACE_SPEAKING)s_mouth_until=n;}
face_state_t display_face_get_state(void){face_state_t s;portENTER_CRITICAL(&s_mux);s=s_state;portEXIT_CRITICAL(&s_mux);return s;}
void display_face_render_mochi_gaze(int expr,int step,int sX,int sY,int gx,int gy,int ex,int ey){memset(s_fb,0,sizeof(s_fb));ears(sY);int l=34+sX+ex,r=94+sX+ex,y=28+sY+ey;if(expr==6){sadbrow(l,14+sY,true);sadbrow(r,14+sY,false);sadeye(l,y,gy);sadeye(r,y,gy);}else if(expr==2&&step==2){happy_eye(l,y);happy_eye(r,y);brow(l,14+sY,1);brow(r,14+sY,1);mouth(2,sY);}else if(expr==99){erroreye(l,y,0);erroreye(r,y,0);brow(l,14+sY,3);brow(r,14+sY,3);mouth(2,sY);}else if(step==3){sleep_eye(l,y);sleep_eye(r,y);mouth(0,sY);}else if(step==1){blink_eye(l,y,0);blink_eye(r,y,0);brow(l,14+sY);brow(r,14+sY);}else{circ(l,y,expr==1?14:13);circ(r,y,expr==1?14:13);circ(l+clampi(gx,-7,7),y+clampi(gy,-5,5),6,false);circ(r+clampi(gx,-7,7),y+clampi(gy,-5,5),6,false);brow(l,14+sY);brow(r,14+sY);if(expr==1){attentive(l,14+sY);attentive(r,14+sY);mouth(1,sY);}else mouth(0,sY);}}
void display_face_render_mochi(int expr,int step,int sX,int sY,int arah){int gx=arah==1?-5:arah==2?5:0,gy=arah==3?-5:0;display_face_render_mochi_gaze(expr,step,sX,sY,gx,gy,0,0);}
void display_face_render(void){render_frame((uint32_t)(esp_timer_get_time()/1000ULL));}
const uint8_t *display_face_buffer(void){return s_fb;}
