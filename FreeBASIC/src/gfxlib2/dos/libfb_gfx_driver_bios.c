/*
 *  libgfx2 - FreeBASIC's alternative gfx library
 *	Copyright (C) 2005 Angelo Mottola (a.mottola@libero.it)
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2.1 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

/*
 * bios.c -- BIOS gfx driver
 *
 * chng: oct/2005 written [mjs]
 *
 */

#include "fb_gfx_dos.h"
#include <assert.h>
#include <go32.h>
#include <pc.h>
#include <dpmi.h>
#include <sys/farptr.h>

typedef void (*fnUpdate)(void);

static int driver_init(char *title, int w, int h, int depth, int refresh_rate, int flags);
static void driver_update_bpp1(void);
static void driver_update_bpp2(void);
static void driver_update_bpp4(void);
static void end_of_driver_update(void);
static int *driver_fetch_modes(int depth, int *size);

GFXDRIVER fb_gfxDriverBIOS =
{
	"BIOS",                   /* char *name; */
	driver_init,             /* int (*init)(char *title, int w, int h, int depth, int refresh_rate, int flags); */
	fb_dos_exit,             /* void (*exit)(void); */
	fb_dos_lock,             /* void (*lock)(void); */
	fb_dos_unlock,           /* void (*unlock)(void); */
	fb_dos_set_palette,      /* void (*set_palette)(int index, int r, int g, int b); */
	fb_dos_vga_wait_vsync,   /* void (*wait_vsync)(void); */
	fb_dos_get_mouse,        /* int (*get_mouse)(int *x, int *y, int *z, int *buttons); */
	fb_dos_set_mouse,        /* void (*set_mouse)(int x, int y, int cursor); */
	fb_dos_set_window_title, /* void (*set_window_title)(char *title); */
	driver_fetch_modes,      /* int *(*fetch_modes)(int depth, int *size); */
	NULL                     /* void (*flip)(void); */
};

static const int res_bpp1[] = {
	SCREENLIST(640, 200),     /* 0x06 */
    SCREENLIST(640, 350),     /* 0x0F */
    SCREENLIST(640, 480),     /* 0x11 */
    0
};
static const unsigned char mode_bpp1[] = {
	0x06, 0x0F, 0x11, 0
};
static const int res_bpp2[] = {
	SCREENLIST(320, 200),     /* 0x04 */
    0
};
static const unsigned char mode_bpp2[] = {
	0x04, 0
};
static const int res_bpp4[] = {
#if 0
	SCREENLIST(320, 200),     /* 0x0D */
	SCREENLIST(640, 200),     /* 0x0E */
	SCREENLIST(640, 350),     /* 0x10 */
    SCREENLIST(640, 480),     /* 0x12 */
#endif
    0
};
static const unsigned char mode_bpp4[] = {
#if 0
    0x0D, 0x0E, 0x10, 0x12,
#endif
    0
};

typedef struct _driver_depth_modes {
    int bit_depth;
    const int * resolutions;
    const unsigned char * modes;
    fnUpdate pfnUpdate;
} driver_depth_modes;

static const driver_depth_modes scr_modes[] = {
    { 1, res_bpp1, mode_bpp1, driver_update_bpp1 },
    { 2, res_bpp2, mode_bpp2, driver_update_bpp2 },
    { 4, res_bpp4, mode_bpp4, driver_update_bpp4 }
};

#define DRV_DEPTH_COUNT (sizeof(scr_modes)/sizeof(scr_modes[0]))

static unsigned char uchScanLineBuffer[ 640 ];

/*:::::*/
static int driver_init(char *title, int w, int h, int depth, int refresh_rate, int flags)
{
    int i;
    unsigned char uchNewMode;
    const driver_depth_modes *depth_modes = NULL;
    int iResCount, iFoundRes;

    fb_dos_detect();

    /* Remove this dumb "fake" scan line size */
    h /= fb_mode->scanline_size;

	if (flags & DRIVER_OPENGL)
		return -1;

    for( i=0; i!=DRV_DEPTH_COUNT; ++i ) {
        if( scr_modes[i].bit_depth == depth ) {
            depth_modes = scr_modes + i;
            break;
        }
    }

    if (depth_modes == NULL)
        return -1;

    iResCount = 0;
    while( depth_modes->resolutions[ iResCount ]!=0 )
        ++iResCount;

    iFoundRes = iResCount;
    for( i=0; i!=iResCount; ++i ) {
        if( depth_modes->resolutions[ i ]==SCREENLIST( w, h ) ) {
            iFoundRes = i;
            break;
        }
    }

    if( iFoundRes==iResCount )
        return -1;

    fb_dos.regs.h.ah = 0x00;
    fb_dos.regs.h.al = depth_modes->modes[ iFoundRes ];
    __dpmi_int(0x10, &fb_dos.regs);

    _movedatab( _dos_ds, 0x449, _my_ds(), (int) &uchNewMode, 1 );
    if( uchNewMode!=depth_modes->modes[ iFoundRes ] )
        return -1;

    /* We only need a scanline_size of 1 because the doubling will be done
     * by the graphics card itself. */
    fb_mode->scanline_size = 1;
    refresh_rate = 70;

	fb_dos.update = depth_modes->pfnUpdate;
	fb_dos.update_len = (unsigned char*)end_of_driver_update - (unsigned char*)fb_dos.update;
    fb_dos.set_palette = NULL;
    fb_dos.bios_mode = depth_modes->modes[ iFoundRes ];

	fb_dos_init(title, w, h, depth, refresh_rate, flags);

	return 0;
}



/*:::::*/
static void driver_update_bpp1(void)
{
    if( fb_dos.bios_mode==0x06 ) {
        /* CGA interlaced mode */
        int y, w = fb_dos.w, w4 = w >> 2, w8 = w >> 3, w32 = w >> 5;
        unsigned int *buffer = (unsigned int *)fb_mode->framebuffer;
        unsigned int screen_even = 0xB8000;
        unsigned int screen_odd  = 0xBA000;

        for (y = 0;
             y != fb_dos.h;
             ++y)
        {
            unsigned int *pScreenOffset = ( ((y & 1)==0) ? &screen_even : &screen_odd );
            if (fb_mode->dirty[y]) {
                unsigned dx = w8;
                unsigned x = w4;
                unsigned char uchDst;
                while( x-- ) {
                    unsigned value = buffer[x];
                    if( x & 1 ) {
                        uchDst = (unsigned char)
                            (
                             ((value & 0x01000000) >> 24) +
                             ((value & 0x00010000) >> 15) +
                             ((value & 0x00000100) >>  6) +
                             ((value & 0x00000001) <<  3)
                            );
                    } else {
                        uchScanLineBuffer[dx--] = (unsigned char)
                            (
                             uchDst +
                             (
                              ((value & 0x01000000) >> 20) +
                              ((value & 0x00010000) >> 11) +
                              ((value & 0x00000100) >>  2) +
                              ((value & 0x00000001) <<  7)
                             )
                            );
                    }
                }
                _movedatal(_my_ds(), (int) uchScanLineBuffer,
                           _dos_ds, *pScreenOffset,
                           w32);
            }
            buffer += w4;
            *pScreenOffset += w8;
        }
    } else {
        /* EGA/VGA linear mode */
        int y, w = fb_dos.w, w4 = w >> 2, w8 = w >> 3, w32 = w >> 5;
        unsigned int *buffer = (unsigned int *)fb_mode->framebuffer;
        unsigned int screen_offset = 0xA0000;

        for (y = 0;
             y != fb_dos.h;
             ++y)
        {
            if (fb_mode->dirty[y]) {
                unsigned dx = w8;
                unsigned x = w4;
                unsigned char uchDst;
                while( x-- ) {
                    unsigned value = buffer[x];
                    if( x & 1 ) {
                        uchDst = (unsigned char)
                            (
                             ((value & 0x01000000) >> 24) +
                             ((value & 0x00010000) >> 15) +
                             ((value & 0x00000100) >>  6) +
                             ((value & 0x00000001) <<  3)
                            );
                    } else {
                        uchScanLineBuffer[dx--] = (unsigned char)
                            (
                             uchDst +
                             (
                              ((value & 0x01000000) >> 20) +
                              ((value & 0x00010000) >> 11) +
                              ((value & 0x00000100) >>  2) +
                              ((value & 0x00000001) <<  7)
                             )
                            );
                    }
                }
                _movedatal(_my_ds(), (int) uchScanLineBuffer,
                           _dos_ds, screen_offset,
                           w32);
            }
            buffer += w4;
            screen_offset += w8;
        }
    }
}

/*:::::*/
static void driver_update_bpp2(void)
{
	int y, w = fb_dos.w, w4 = w >> 2, w16 = w >> 4;
	unsigned int *buffer = (unsigned int *)fb_mode->framebuffer;
    unsigned int screen_even = 0xB8000;
    unsigned int screen_odd  = 0xBA000;

    for (y = 0;
         y != fb_dos.h;
         ++y)
    {
        unsigned int *pScreenOffset = ( ((y & 1)==0) ? &screen_even : &screen_odd );
        if (fb_mode->dirty[y]) {
            unsigned x = w4;
            while( x-- ) {
                unsigned value = buffer[x];
                uchScanLineBuffer[x] = (unsigned char)
                    (
                     ((value & 0x03000000) >> 24) +
                     ((value & 0x00030000) >> 14) +
                     ((value & 0x00000300) >>  4) +
                     ((value & 0x00000003) <<  6)
                    );
            }
            _movedatal(_my_ds(), (int) uchScanLineBuffer,
                       _dos_ds, *pScreenOffset,
                       w16);
        }
        buffer += w4;
        *pScreenOffset += w4;
    }
}

/*:::::*/
static void driver_update_bpp4(void)
{
    /* FIXME: This has to be implemented soon (to support SCREEN 12
     * and others) */
}

static void end_of_driver_update(void) { /* do not remove */ }

/*:::::*/
static int *driver_fetch_modes(int depth, int *size)
{
    int i;
    const driver_depth_modes *depth_modes = NULL;
    int iResCount;
#ifdef DEBUG
    int iModeCount;
#endif
    for( i=0; i!=DRV_DEPTH_COUNT; ++i ) {
        if( scr_modes[i].bit_depth == depth ) {
            depth_modes = scr_modes + i;
            break;
        }
    }

    if (depth_modes == NULL)
        return NULL;

    iResCount = 0;
    while( depth_modes->resolutions[ iResCount ]!=0 )
        ++iResCount;

#ifdef DEBUG
    iModeCount = 0;
    while( depth_modes->modes[ iModeCount ]!=0 )
        ++iModeCount;
    assert( iModeCount==iResCount );
#endif

	*size = iResCount;
    return memcpy((void*)malloc(iResCount * sizeof( int )),
                  depth_modes->resolutions, iResCount * sizeof( int ));
}
