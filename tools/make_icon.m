#import <AppKit/AppKit.h>
int main(int argc, const char *argv[]) {
    @autoreleasepool {
        if (argc != 2) return 1;
        NSString *directory = @(argv[1]);
        for (NSNumber *sizeValue in @[@16, @32, @128, @256, @512]) {
            int size = sizeValue.intValue;
            for (int scale = 1; scale <= 2; scale++) {
                int pixels = size * scale;
                NSBitmapImageRep *rep = [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:NULL
                    pixelsWide:pixels pixelsHigh:pixels bitsPerSample:8 samplesPerPixel:4
                    hasAlpha:YES isPlanar:NO colorSpaceName:NSDeviceRGBColorSpace bytesPerRow:0 bitsPerPixel:0];
                [NSGraphicsContext saveGraphicsState];
                [NSGraphicsContext setCurrentContext:[NSGraphicsContext graphicsContextWithBitmapImageRep:rep]];
                NSAffineTransform *transform = [NSAffineTransform transform];
                [transform scaleBy:pixels / 512.0];
                [transform concat];
                [[NSColor colorWithRed:0.12 green:0.14 blue:0.24 alpha:1] setFill];
                [[NSBezierPath bezierPathWithRoundedRect:NSMakeRect(20,20,472,472) xRadius:105 yRadius:105] fill];
                [[NSColor colorWithRed:0.45 green:0.9 blue:0.78 alpha:1] setFill];
                [[NSBezierPath bezierPathWithRoundedRect:NSMakeRect(82,145,348,225) xRadius:30 yRadius:30] fill];
                [[NSColor colorWithRed:0.12 green:0.14 blue:0.24 alpha:1] setFill];
                for (int row=0; row<3; row++) for (int col=0; col<6; col++)
                    [[NSBezierPath bezierPathWithRoundedRect:NSMakeRect(106+col*51, 254-row*45, 35, 29) xRadius:6 yRadius:6] fill];
                [[NSBezierPath bezierPathWithRoundedRect:NSMakeRect(157,159,188,23) xRadius:6 yRadius:6] fill];
                [NSGraphicsContext restoreGraphicsState];
                NSString *name = [NSString stringWithFormat:@"icon_%dx%d%@.png",size,size,scale==2?@"@2x":@""];
                [[rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}]
                    writeToFile:[directory stringByAppendingPathComponent:name] atomically:YES];
            }
        }
    }
    return 0;
}
