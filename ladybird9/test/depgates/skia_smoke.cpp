// skia_smoke.cpp — depgate for the skia sysroot artifact (build-skia.sh).
// CPU raster only: wrap a raw pixel buffer the way Ladybird's
// PaintingSurface::wrap_bitmap does (SkSurfaces::WrapPixels), draw a rect,
// and self-verify the pixels. Compile+link with cc9-c++ / ld.lld; run on
// a 9front box. Verified PASS on bare-metal cirno 2026-07-15.
//
//   CFLAGS="$(PKG_CONFIG_LIBDIR=$SYS/lib/pkgconfig pkg-config --cflags skia)"
//   cc9-c++ -O2 $CFLAGS -c skia_smoke.cpp -o skia_smoke.o
//   ld.lld -o skia_smoke skia_smoke.o --start-group $SYS/lib/libskia.a \
//     $SYS/lib/libskcms.a $SYS/lib/libfreetype.a $SYS/lib/libpng.a \
//     $SYS/lib/libz.a $CC9/lib/libcc9cxx.a $CC9/lib/libcc9m.a --end-group \
//     -T $CC9/test/plan9.ld -static -nostdlib
//   elf2aout.py skia_smoke skia_smoke.aout; ship.py skia_smoke.aout ...
//   # over listen1 run DETACHED — attached to the socket, exit tears down
//   # the whole connection before any output is delivered (notepg family):
//   @{rfork s; /tmp/skia_smoke >/tmp/sk.out >[2=1] &} ... cat /tmp/sk.out
#include <core/SkCanvas.h>
#include <core/SkColor.h>
#include <core/SkColorSpace.h>
#include <core/SkImageInfo.h>
#include <core/SkPaint.h>
#include <core/SkRect.h>
#include <core/SkSurface.h>

#include <stdio.h>
#include <stdint.h>
#include <string.h>

int main()
{
    enum { W = 64, H = 64 };
    static uint32_t pixels[W * H];
    memset(pixels, 0, sizeof(pixels));

    SkImageInfo info = SkImageInfo::Make(W, H, kBGRA_8888_SkColorType, kPremul_SkAlphaType,
                                         SkColorSpace::MakeSRGB());
    sk_sp<SkSurface> surface = SkSurfaces::WrapPixels(info, pixels, W * 4);
    if (!surface) {
        printf("FAIL: WrapPixels returned null\n");
        return 1;
    }

    SkCanvas* canvas = surface->getCanvas();
    canvas->clear(SK_ColorWHITE);
    SkPaint paint;
    paint.setColor(SK_ColorRED);
    paint.setAntiAlias(false);
    canvas->drawRect(SkRect::MakeXYWH(8, 8, 32, 32), paint);

    // BGRA8888 premul: red = B=0x00 G=0x00 R=0xff A=0xff -> LE word 0xffff0000
    uint32_t inside = pixels[16 * W + 16];
    uint32_t outside = pixels[2 * W + 2];
    if (inside != 0xffff0000u || outside != 0xffffffffu) {
        printf("FAIL: inside=%08x (want ffff0000) outside=%08x (want ffffffff)\n",
               inside, outside);
        return 1;
    }
    printf("PASS: skia CPU raster rect over WrapPixels buffer\n");
    return 0;
}
