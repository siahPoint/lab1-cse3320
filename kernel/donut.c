/*
    Draw a rotating donut on framebuffer or over uart. 
    use ktimer for timing. more efficient. 

    dependency:
        ktimer 
        fb (uart may work? depending on terminal program)
*/

#include "debug.h"
#include "plat.h"
#include "utils.h"

///////////////////////////////////////////////////////////////////////////////

#define PIXELSIZE 4 /*ARGB, expected by /dev/fb*/
typedef unsigned int PIXEL;
#define NN 640 // canvas dimension. NN by NN

#include "fb.h"
static inline void setpixel(unsigned char *buf, int x, int y, int pit, PIXEL p) {
    assert(x >= 0 && y >= 0);
    *(PIXEL *)(buf + y * pit + x * PIXELSIZE) = p;
}

static void canvas_init(void) {
    fb_fini();
    // acquire(&mboxlock);      //it's a test. so no lock

    the_fb.width = NN;
    the_fb.height = NN;

    the_fb.vwidth = NN;
    the_fb.vheight = NN;

    if (fb_init() != 0)
        BUG();
}

#define R(mul, shift, x, y)              \
    _ = x;                               \
    x -= mul * y >> shift;               \
    y += mul * _ >> shift;               \
    _ = (3145728 - x * x - y * y) >> 11; \
    x = x * _ >> 10;                     \
    y = y * _ >> 10;

static char b[1760];        // text buffer (W 80 H 22?
static signed char z[1760]; // z buffer
static int sA = 1024; 
static int cA = 0; 
static int sB = 1024; 
static int cB = 0; 
static int _;

static PIXEL int2rgb (int value); 

// ktimer callback. NB: called in irq context
static int draw_frame(TKernelTimerHandle hTimer, void *param, void *context) {    
    memset(b, 0, 1760);  // text buffer 0: black bkgnd
    memset(z, 127, 1760); // z buffer
    int sj = 0, cj = 1024;
    for (int j = 0; j < 90; j++) {
        int si = 0, ci = 1024; // sine and cosine of angle i
        for (int i = 0; i < 324; i++) {
            int R1 = 1, R2 = 2048, K2 = 5120 * 1024;

            int x0 = R1 * cj + R2,
                x1 = ci * x0 >> 10,
                x2 = cA * sj >> 10,
                x3 = si * x0 >> 10,
                x4 = R1 * x2 - (sA * x3 >> 10),
                x5 = sA * sj >> 10,
                x6 = K2 + R1 * 1024 * x5 + cA * x3,
                x7 = cj * si >> 10,
                x = 25 + 30 * (cB * x1 - sB * x4) / x6,
                y = 12 + 15 * (cB * x4 + sB * x1) / x6,
                // N = (((-cA * x7 - cB * ((-sA * x7 >> 10) + x2) - ci * (cj * sB >> 10)) >> 10) - x5) >> 7;
                lumince = (((-cA * x7 - cB * ((-sA * x7 >> 10) + x2) - ci * (cj * sB >> 10)) >> 10) - x5); 
                // range likely: <0..~1408, scale to 0..255
                lumince = lumince<0? 0 : lumince/5; 
                lumince = lumince<255? lumince : 255; 

            int o = x + 80 * y; // fxl: 80 chars per row
            signed char zz = (x6 - K2) >> 15;
            if (22 > y && y > 0 && x > 0 && 80 > x && zz < z[o]) { // fxl: z depth will control visibility
                z[o] = zz;
                // luminance_index is now in the range 0..11 (8*sqrt(2) = 11.3)
                // now we lookup the character corresponding to the
                // luminance and plot it in our output:
                // b[o] = ".,-~:;=!*#$@"[N > 0 ? N : 0];
                b[o] = lumince;                    
            }
            R(5, 8, ci, si) // rotate i
        }
        R(9, 7, cj, sj) // rotate j
    }
    R(5, 7, cA, sA);
    R(5, 8, cB, sB);

    int y = 0, x = 0;
    for (int k = 0; 1761 > k; k++) {
        if (k % 80) {
            if (x < 50) {
                // to display, scale x by K, y by 2K (so we have a round donut)
                int K=6, xx=x*K, yy=y*K*2;
                // PIXEL clr = b[k]; // blue only, simple
                PIXEL clr = int2rgb(b[k]); // to a color spectrum
                setpixel(the_fb.fb, xx, yy, the_fb.pitch, clr);
                setpixel(the_fb.fb, xx+1, yy, the_fb.pitch, clr);
                setpixel(the_fb.fb, xx, yy+1, the_fb.pitch, clr);
                setpixel(the_fb.fb, xx+1, yy+1, the_fb.pitch, clr);
            }
            x++;
        } else { 
            y++;
            x = 1;
        }
    }

    return 1; // restart timer 
}

void donut(void) {
    int ret; 
    canvas_init();
    ret = ktimer_start(100, /*firing interval, ms*/
        draw_frame, 0 /*args*/, 0 /* context */); 
    BUG_ON(ret<0);     
}

/* simple way to drive animation, NOT using vtimer. just hw irq 
    to enable it,  irq handler must be modified to call sys_timer_irq_simple()    
*/
void donut_simple(void) {
    canvas_init();
    put32(TIMER_C1, 100 * 1000);	// in us
}

// // copied from timer.c. dirty. just to verify that this works
// static inline unsigned long current_counter() {
// 	// assume these two are consistent, since the clock is only 1MHz...
// 	return ((unsigned long) get32(TIMER_CHI) << 32) | get32(TIMER_CLO); 
// }

extern unsigned long current_counter(); // timer.c, dirty. 

//quest: Pixel donut. 
void sys_timer_irq_simple(void) 
{
	unsigned long cur; 
    BUG_ON(!(get32(TIMER_CS) & TIMER_CS_M1));  
	put32(TIMER_CS, TIMER_CS_M1);	// clear timer1 match
    draw_frame(0, 0, 0); 
    cur = current_counter(); 
    // reset the timer to fire in the future
    put32(TIMER_C1, cur + 100 * 1000 /*in us*/);
}

///////////////////////////////////////////////////////////////////////////////


/* same as above, but print chars to uart. use delay() -- inefficient
    qemu: ok
    rpi3 hw: ok. need a modern terminal program (e.g. putty) that can interpret
    special chars */
void donut_text(void) {
    int sA = 1024, cA = 0, sB = 1024, cB = 0, _;

    while (1) {
        memset(b, 32, 1760);  // text buffer
        memset(z, 127, 1760); // z buffer
        int sj = 0, cj = 1024;
        for (int j = 0; j < 90; j++) {
            int si = 0, ci = 1024; // sine and cosine of angle i
            for (int i = 0; i < 324; i++) {
                int R1 = 1, R2 = 2048, K2 = 5120 * 1024;

                int x0 = R1 * cj + R2,
                    x1 = ci * x0 >> 10,
                    x2 = cA * sj >> 10,
                    x3 = si * x0 >> 10,
                    x4 = R1 * x2 - (sA * x3 >> 10),
                    x5 = sA * sj >> 10,
                    x6 = K2 + R1 * 1024 * x5 + cA * x3,
                    x7 = cj * si >> 10,
                    x = 25 + 30 * (cB * x1 - sB * x4) / x6,
                    y = 12 + 15 * (cB * x4 + sB * x1) / x6,
                    N = (((-cA * x7 - cB * ((-sA * x7 >> 10) + x2) - ci * (cj * sB >> 10)) >> 10) - x5) >> 7;

                int o = x + 80 * y; // fxl: 80 chars per row
                signed char zz = (x6 - K2) >> 15;
                if (22 > y && y > 0 && x > 0 && 80 > x && zz < z[o]) {
                    z[o] = zz;
                    // quest: change luminance of Donut 
                    // luminance_index is now in the range 0..11 (8*sqrt(2) = 11.3)
                    // now we lookup the character corresponding to the
                    // luminance and plot it in our output:
                    b[o] = ".,-~:;=!*#$@"[N > 0 ? N : 0];
                }
                R(5, 8, ci, si) // rotate i
            }
            R(9, 7, cj, sj) // rotate j
        }
        R(5, 7, cA, sA);
        R(5, 8, cB, sB);

        for (int k = 0; 1761 > k; k++)
            putc(0, k % 80 ? b[k] : 10);
        printf("\x1b[23A");  // clear console 
        ms_delay(10);  // can delay in this way, but inefficient
    }
}

// map luminance [0..255] to rgb color
// value: 0..255, PIXEL: argb
static PIXEL int2rgb (int value) {
    int r,g,b;     
    if (value >= 0 && value <= 85) {
        // Black to Yellow (R stays 0, G increases, B stays 0)
        r = 0;
        g = (value * 3);
        b = 0;
    } else if (value > 85 && value <= 170) {
        // Yellow to Cyan (G stays 255, R decreases, B increases)
        r = 255 - ((value - 85) * 3);
        g = 255;
        b = (value - 85) * 3;
    } else if (value > 170 && value <= 255) {
        // Cyan to Blue (G decreases, B stays 255, R stays 0)
        r = 0;
        g = 255 - ((value - 170) * 3);
        b = 255;
    } else {
        // Value out of range
        r=g=b=0;
    }    
    return (r<<16)|(g<<8)|b; 
}

/// project idea: use ktimer to drive the animation. periodically

/**             CREDITS 
 * Original author:
 * https://twitter.com/a1k0n
 * https://www.a1k0n.net/2021/01/13/optimizing-donut.html
 *
 * Change Logs:
 * Date           Author       Notes
 * 2006-09-15     Andy Sloane  First version
 * 2011-07-20     Andy Sloane  Second version
 * 2021-01-13     Andy Sloane  Third version
 * 2021-03-25     Meco Man     Port to RT-Thread RTOS
 *
 *
 *  js code for both canvas & text version
 *  https://www.a1k0n.net/js/donut.js
 *
 *  ported by FL
 * From the NJU OS project:
 * https://github.com/NJU-ProjectN/am-kernels/blob/master/kernels/demo/src/donut/donut.c
 *
 */
