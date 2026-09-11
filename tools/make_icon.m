#import <AppKit/AppKit.h>

static void appendICNSElement(NSMutableData *iconData, const char type[4], NSData *pngData) {
    uint32_t length = CFSwapInt32HostToBig((uint32_t)(8 + pngData.length));
    [iconData appendBytes:type length:4];
    [iconData appendBytes:&length length:sizeof(length)];
    [iconData appendData:pngData];
}

static NSData *iconPNG(int pixels) {
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
    for (int row = 0; row < 3; row++) for (int col = 0; col < 6; col++)
        [[NSBezierPath bezierPathWithRoundedRect:NSMakeRect(106 + col * 51, 254 - row * 45, 35, 29) xRadius:6 yRadius:6] fill];
    [[NSBezierPath bezierPathWithRoundedRect:NSMakeRect(157,159,188,23) xRadius:6 yRadius:6] fill];
    [NSGraphicsContext restoreGraphicsState];
    return [rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
}

int main(int argc, const char *argv[]) {
    @autoreleasepool {
        if (argc != 2) return 1;
        NSArray<NSArray<id> *> *elements = @[
            @[@"icp4", @16], @[@"icp5", @32], @[@"icp6", @64], @[@"ic07", @128],
            @[@"ic08", @256], @[@"ic09", @512], @[@"ic10", @1024],
        ];
        NSMutableData *iconData = [NSMutableData dataWithBytes:"icns\0\0\0\0" length:8];
        for (NSArray<id> *element in elements) {
            NSData *pngData = iconPNG([element[1] intValue]);
            if (pngData == nil) return 1;
            appendICNSElement(iconData, [element[0] UTF8String], pngData);
        }
        uint32_t totalLength = CFSwapInt32HostToBig((uint32_t)iconData.length);
        [iconData replaceBytesInRange:NSMakeRange(4, sizeof(totalLength)) withBytes:&totalLength];
        if (![iconData writeToFile:@(argv[1]) atomically:YES]) return 1;
    }
    return 0;
}
