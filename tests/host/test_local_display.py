#!/usr/bin/env python3
"""Host checks of production channel mapping, RTP progress and display state.
Hardware/GDS/RTOS operations are stubs; this is not a firmware or visual QA run.
"""
from pathlib import Path
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[2]
def extract(text, signature):
    start = text.index(signature)
    end = text.index('{', start) + 1
    depth = 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end] + '\n'
common = '''
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
'''
raop = (ROOT/'components/raop/raop.c').read_text()
progress = common + extract(raop, 'static bool parse_progress(') + '''
int main(void) {
 int e,d;
 assert(parse_progress("progress: 100000/122050/188200",&e,&d) && e==500 && d==2000);
 assert(parse_progress("progress: 4294967000/43804/87904",&e,&d) && e==1000 && d==2000);
 assert(parse_progress("progress: 100000/90000/188200",&e,&d) && e==0 && d==2000);
 assert(parse_progress("progress: 100000/122050/0",&e,&d) && d==0);
 assert(!parse_progress("progress: broken",&e,&d));
 puts("PASS RTP timestamps: precision, wrap, pre-start, unknown duration, malformed");
}
'''
src = (ROOT/'components/squeezelite/output_i2s.c').read_text()
start = src.index('static int _i2s_write_frames(',src.index('bool output_volume_i2s'))
audio = common + '''
#if BYTES_PER_FRAME == 4
 typedef int16_t ISAMPLE_T;
#else
 typedef int32_t ISAMPLE_T;
#endif
typedef uint32_t frames_t;
typedef int32_t s32_t;
typedef uint8_t u8_t;
#define FADE_ACTIVE 1
#define FADE_CROSS 1
#define _apply_gain(...) ((void)0)
#define _apply_cross(...) ((void)0)
#define output_visu_export(...) ((void)0)
static unsigned used;
#define _buf_used(x) used
static struct {int fade,fade_dir; unsigned current_sample_rate;} output;
static struct {u8_t *readp;} fifo;
#define outputbuf (&fifo)
static struct {bool enabled;} spdif;
static ISAMPLE_T destination[8], quiet[8];
static u8_t *obuf=(u8_t *)destination,*silencebuf=(u8_t *)quiet;
static frames_t oframes;
''' + extract(src[start:],'static int _i2s_write_frames(') + '''
int main(void) {
 ISAMPLE_T pcm[]={111,-222,333,-444}, *cross=NULL;
 fifo.readp=(u8_t *)pcm;
 for(int mode=0; mode<3; ++mode) {
  memset(destination,0x55,sizeof(destination));
  ISAMPLE_T sentinel=destination[0];
  oframes=1; spdif.enabled=(mode==1);
  assert(_i2s_write_frames(2,mode==2,0,0,0,0,0,&cross)==2);
  assert(oframes==3 && destination[0]==sentinel && pcm[0]==111 && pcm[1]==-222);
  if(mode==0) assert(destination[2]==-222 && destination[3]==111 && destination[4]==-444 && destination[5]==333);
  if(mode==1) assert(destination[2]==111 && destination[3]==-222);
  if(mode==2) assert(destination[2]==0 && destination[3]==0);
 }
 puts("PASS DAC swap, SPDIF bypass, silence, FIFO and preceding frames preserved");
}
'''
src = (ROOT/'components/squeezelite/displayer.c').read_text()
globals_ = src[src.index('#define LOCAL_TITLE_MAX'):src.index('static void server(')]
prelude = common + '''
typedef uint32_t TickType_t;
static TickType_t ticks;
#define pdMS_TO_TICKS(x) ((x)/10)
#define pdTICKS_TO_MS(x) ((uint32_t)(x)*10)
#define xTaskGetTickCount() ticks
#define portMAX_DELAY 0
#define xSemaphoreTake(...) ((void)0)
#define xSemaphoreGive(...) ((void)0)
#define vTaskResume(...) ((void)0)
#define min(a,b) ((a)<(b)?(a):(b))
#define GDS_COLOR_BLACK 0
#define GDS_COLOR_WHITE 1
static int device, Font_droid_sans_fallback_24x28;
static void *display=&device;
static struct {int mutex,task,wake;} displayer={1,1,0};
static int title_x, title_y, clears;
#define GDS_GetWidth(d) 320
#define GDS_GetHeight(d) 240
#define GDS_SetFont(...) ((void)(&Font_droid_sans_fallback_24x28))
#define GDS_FontMeasureString(d,s) ((int)strlen(s)*10)
#define GDS_FontGetHeight(d) 28
static void GDS_ClearWindow(void *d,int x,int y,int x2,int y2,int c) {
 (void)d;(void)c;
 assert(x>=0 && x2<320 && y>=0 && y2<80 && x<=x2 && y<=y2); ++clears;
}
static void GDS_DrawLine(void *d,int x,int y,int x2,int y2,int c) {
 (void)d;(void)c;assert(x>=0 && x2<320 && y>=0 && y2<80);
}
static void GDS_FontDrawString(void *d,int x,int y,const char *s,int c) {
 (void)d;(void)s;(void)c;title_x=x;title_y=y;
}
'''
methods=''.join(extract(src,x) for x in [
 'void displayer_local_title(', 'static uint32_t local_elapsed_now(',
 'void displayer_local_progress(', 'void displayer_local_playing(',
 'static void local_progress_draw(', 'static void local_title_draw('])
display_test=prelude+globals_+methods+'''
int main(void) {
 displayer_local_title("Short song");local_title_draw();
 assert(title_x==110 && title_y==18 && !local_title_scrolling);
 displayer_local_title("This title is longer than the available width");local_title_draw();
 assert(title_x==8 && local_title_scrolling);
 displayer_local_progress(0,10000);displayer_local_playing(true);
 ticks=500; local_progress_draw();assert(local_progress_pixels==152);
 int before=clears;local_progress_draw();assert(clears==before);
 displayer_local_playing(false);ticks=700;assert(local_elapsed_now(ticks)==5000);
 displayer_local_progress(2000,10000);assert(local_elapsed_now(ticks)==2000);
 displayer_local_playing(true);ticks=800;assert(local_elapsed_now(ticks)==3000);
 ticks=2000;local_progress_draw();assert(local_progress_pixels==304);
 displayer_local_progress(0,0);local_progress_draw();assert(local_progress_pixels==0);
 ticks=UINT32_MAX-5;displayer_local_progress(0,10000);ticks=4;
 assert(local_elapsed_now(ticks)==100);
 puts("PASS title centering/scroll, bar bounds, pause/resume/seek/end, tick wrap, no redundant redraw");
}
'''
with tempfile.TemporaryDirectory(prefix='local-display-test-') as tmp:
 for name,code,flags in [('progress',progress,[]),('audio16',audio,['-DBYTES_PER_FRAME=4']),('audio32',audio,['-DBYTES_PER_FRAME=8']),('display',display_test,[])]:
  path=Path(tmp)/name;path.with_suffix('.c').write_text(code)
  subprocess.run(['gcc','-std=gnu99','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-fsanitize=address,undefined',*flags,str(path.with_suffix('.c')),'-o',str(path)],check=True)
  subprocess.run([str(path)],check=True)
