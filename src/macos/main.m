#import <AppKit/AppKit.h>
#import <ApplicationServices/ApplicationServices.h>
#import <IOKit/hidsystem/IOHIDLib.h>

#include "haptik_audio.h"
#include "haptik_impact.h"
#include "haptik_sensor.h"

#include <mach/mach_time.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdint.h>

@interface HaptikAppDelegate : NSObject <NSApplicationDelegate>
- (NSArray<NSDictionary *> *)availableSoundPacks;
- (BOOL)loadSoundPackID:(NSString *)packID startAudio:(BOOL)startAudio;
@end

static const char *HAPTIK_DEBUG_LOG = "/private/tmp/haptik-debug.log";
static double monotonic_seconds(void);

static NSString *HL(NSString *key) {
    return NSLocalizedString(key, nil);
}

static void haptik_debug_log(const char *format, ...) {
    FILE *file = fopen(HAPTIK_DEBUG_LOG, "a");
    if (file == NULL) {
        return;
    }
    fprintf(file, "[%.3f] ", monotonic_seconds());
    va_list arguments;
    va_start(arguments, format);
    vfprintf(file, format, arguments);
    va_end(arguments);
    fputc('\n', file);
    fclose(file);
}

static double monotonic_seconds(void) {
    static mach_timebase_info_data_t timebase;
    static dispatch_once_t once_token;
    dispatch_once(&once_token, ^{
        (void)mach_timebase_info(&timebase);
    });
    return (double)mach_absolute_time() *
        ((double)timebase.numer / (double)timebase.denom) * 1e-9;
}

@implementation HaptikAppDelegate {
    NSWindow *_window;
    NSTextField *_diagnosticsLabel;
    uint_fast64_t _previousSampleCount;
    unsigned _stalledChecks;
    NSStatusItem *_statusItem;
    NSMenuItem *_statusMenuItem;
    NSMenuItem *_enabledMenuItem;
    NSMenuItem *_permissionMenuItem;
    NSArray<NSMenuItem *> *_volumeMenuItems;
    NSArray<NSMenuItem *> *_sensitivityMenuItems;
    NSArray<NSMenuItem *> *_soundMenuItems;

    haptik_sensor_t *_sensor;
    haptik_impact_detector_t _impactDetector;
    haptik_audio_t *_audio;
    CFMachPortRef _eventTap;
    CFRunLoopSourceRef _eventTapSource;
    id _globalKeyMonitor;
    NSTimer *_permissionRetryTimer;
    atomic_uint_fast64_t _sensorSampleCount;
    atomic_uint_fast64_t _keyEventCount;

    BOOL _enabled;
    float _volume;
    float _sensitivity;
    NSString *_sensorStatus;
    NSString *_keyboardStatus;
    NSString *_soundStatus;
    NSString *_selectedSoundPackID;
}

static void sensor_sample_callback(
    const haptik_accel_sample_t *sample,
    void *context
) {
    HaptikAppDelegate *delegate = (__bridge HaptikAppDelegate *)context;
    haptik_impact_push_sample(&delegate->_impactDetector, sample);
    const uint_fast64_t count = atomic_fetch_add_explicit(
        &delegate->_sensorSampleCount,
        1,
        memory_order_relaxed
    ) + 1U;
    if (count == 1U) {
        haptik_debug_log("first accelerometer sample received");
    }
}

static CGEventRef keyboard_event_callback(
    CGEventTapProxy proxy,
    CGEventType type,
    CGEventRef event,
    void *context
) {
    (void)proxy;
    HaptikAppDelegate *delegate = (__bridge HaptikAppDelegate *)context;
    if (type == kCGEventTapDisabledByTimeout ||
        type == kCGEventTapDisabledByUserInput) {
        if (delegate->_eventTap != NULL) {
            CGEventTapEnable(delegate->_eventTap, true);
        }
        return event;
    }
    if (type != kCGEventKeyDown || !delegate->_enabled) {
        return event;
    }

    const uint16_t keyCode = (uint16_t)CGEventGetIntegerValueField(
        event,
        kCGKeyboardEventKeycode
    );
    const float intensity = haptik_impact_intensity_at(
        &delegate->_impactDetector,
        monotonic_seconds()
    );
    haptik_audio_trigger(delegate->_audio, keyCode, intensity);
    const uint_fast64_t count = atomic_fetch_add_explicit(
        &delegate->_keyEventCount,
        1,
        memory_order_relaxed
    ) + 1U;
    if (count <= 3U) {
        haptik_debug_log("keyboard event received (count=%llu)",
            (unsigned long long)count);
    }
    return event;
}

- (void)applicationDidFinishLaunching:(NSNotification *)notification {
    (void)notification;
    [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];

    FILE *debugFile = fopen(HAPTIK_DEBUG_LOG, "w");
    if (debugFile != NULL) {
        fclose(debugFile);
    }
    atomic_init(&_sensorSampleCount, 0);
    atomic_init(&_keyEventCount, 0);
    haptik_debug_log(
        "launch: listen=%d accessibility=%d hid=%d sensor_present=%d",
        CGPreflightListenEventAccess(),
        AXIsProcessTrusted(),
        (int)haptik_sensor_check_access(),
        haptik_sensor_available()
    );

    NSUserDefaults *defaults = NSUserDefaults.standardUserDefaults;
    [defaults registerDefaults:@{
        @"enabled": @YES,
        @"volume": @0.65,
        @"sensitivity": @1.0,
        @"soundPack": @"kailh_white"
    }];
    _enabled = [defaults boolForKey:@"enabled"];
    _volume = [defaults floatForKey:@"volume"];
    _sensitivity = [defaults floatForKey:@"sensitivity"];
    _selectedSoundPackID = [defaults stringForKey:@"soundPack"];
    _sensorStatus = HL(@"status.sensor.starting");
    _keyboardStatus = HL(@"status.keyboard.checking");
    _soundStatus = HL(@"status.sound.loading");

    haptik_impact_init(&_impactDetector);
    haptik_impact_set_sensitivity(&_impactDetector, _sensitivity);
    _audio = haptik_audio_create();
    if (_audio != NULL) {
        if (![self loadSoundPackID:_selectedSoundPackID startAudio:NO]) {
            _selectedSoundPackID = @"kailh_white";
            (void)[self loadSoundPackID:_selectedSoundPackID startAudio:NO];
        }
        haptik_audio_set_volume(_audio, _volume);
        if (!haptik_audio_start(_audio)) {
            _sensorStatus = [NSString stringWithFormat:
                HL(@"status.audio.error.format"),
                haptik_audio_last_error(_audio)];
            haptik_debug_log("audio start failed: %s",
                haptik_audio_last_error(_audio));
        } else {
            haptik_debug_log("audio engine started");
        }
    } else {
        _sensorStatus = HL(@"status.audio.creation_failed");
    }

    [self buildStatusMenu];
    [self startKeyboardCapturePrompting:YES];
    [self startSensorRequestingAccess:YES];
    _permissionRetryTimer = [NSTimer
        scheduledTimerWithTimeInterval:2.0
        target:self
        selector:@selector(retryPermissionsIfNeeded:)
        userInfo:nil
        repeats:YES];
    [NSWorkspace.sharedWorkspace.notificationCenter addObserver:self
        selector:@selector(systemDidWake:) name:NSWorkspaceDidWakeNotification object:nil];
    [self showWindow:nil];
    [self refreshMenu];
}

- (void)applicationWillTerminate:(NSNotification *)notification {
    (void)notification;
    [_permissionRetryTimer invalidate];
    _permissionRetryTimer = nil;
    if (_globalKeyMonitor != nil) {
        [NSEvent removeMonitor:_globalKeyMonitor];
        _globalKeyMonitor = nil;
    }
    if (_eventTapSource != NULL) {
        CFRunLoopRemoveSource(
            CFRunLoopGetMain(),
            _eventTapSource,
            kCFRunLoopCommonModes
        );
        CFRelease(_eventTapSource);
        _eventTapSource = NULL;
    }
    if (_eventTap != NULL) {
        CFMachPortInvalidate(_eventTap);
        CFRelease(_eventTap);
        _eventTap = NULL;
    }
    haptik_sensor_destroy(_sensor);
    _sensor = NULL;
    haptik_audio_destroy(_audio);
    _audio = NULL;
    haptik_debug_log("application terminated");
}

- (void)buildStatusMenu {
    _statusItem = [NSStatusBar.systemStatusBar
        statusItemWithLength:NSVariableStatusItemLength];
    _statusItem.button.title = @"⌨︎";
    _statusItem.button.toolTip = @"Haptik";

    NSMenu *menu = [[NSMenu alloc] initWithTitle:@"Haptik"];
    _statusMenuItem = [[NSMenuItem alloc]
        initWithTitle:HL(@"menu.starting")
        action:nil
        keyEquivalent:@""];
    _statusMenuItem.enabled = NO;
    [menu addItem:_statusMenuItem];
    NSMenuItem *windowItem = [[NSMenuItem alloc] initWithTitle:HL(@"menu.open")
        action:@selector(showWindow:) keyEquivalent:@""];
    windowItem.target = self;
    [menu addItem:windowItem];
    [menu addItem:NSMenuItem.separatorItem];

    _enabledMenuItem = [[NSMenuItem alloc]
        initWithTitle:HL(@"menu.enable_sounds")
        action:@selector(toggleEnabled:)
        keyEquivalent:@""];
    _enabledMenuItem.target = self;
    [menu addItem:_enabledMenuItem];

    NSMenuItem *soundRoot = [[NSMenuItem alloc]
        initWithTitle:HL(@"menu.switch_sound")
        action:nil
        keyEquivalent:@""];
    NSMenu *soundMenu = [[NSMenu alloc] initWithTitle:HL(@"menu.switch_sound")];
    NSMutableArray<NSMenuItem *> *soundItems = [NSMutableArray array];
    for (NSDictionary *pack in [self availableSoundPacks]) {
        NSMenuItem *item = [[NSMenuItem alloc]
            initWithTitle:pack[@"title"]
            action:@selector(selectSoundPack:)
            keyEquivalent:@""];
        item.target = self;
        item.representedObject = pack[@"id"];
        [soundMenu addItem:item];
        [soundItems addObject:item];
    }
    _soundMenuItems = soundItems.copy;
    soundRoot.submenu = soundMenu;
    [menu addItem:soundRoot];

    NSMenuItem *volumeRoot = [[NSMenuItem alloc]
        initWithTitle:HL(@"menu.volume")
        action:nil
        keyEquivalent:@""];
    NSMenu *volumeMenu = [[NSMenu alloc] initWithTitle:HL(@"menu.volume")];
    NSMutableArray<NSMenuItem *> *volumeItems = [NSMutableArray array];
    for (NSNumber *number in @[@0.25, @0.50, @0.75, @1.00]) {
        NSMenuItem *item = [[NSMenuItem alloc]
            initWithTitle:[NSString stringWithFormat:@"%%%d",
                (int)(number.floatValue * 100.0F)]
            action:@selector(setVolume:)
            keyEquivalent:@""];
        item.target = self;
        item.representedObject = number;
        [volumeMenu addItem:item];
        [volumeItems addObject:item];
    }
    _volumeMenuItems = volumeItems.copy;
    volumeRoot.submenu = volumeMenu;
    [menu addItem:volumeRoot];

    NSMenuItem *sensitivityRoot = [[NSMenuItem alloc]
        initWithTitle:HL(@"menu.sensitivity")
        action:nil
        keyEquivalent:@""];
    NSMenu *sensitivityMenu = [[NSMenu alloc] initWithTitle:HL(@"menu.sensitivity")];
    NSArray<NSDictionary *> *sensitivityChoices = @[
        @{@"title": HL(@"choice.low"), @"value": @0.65},
        @{@"title": HL(@"choice.normal"), @"value": @1.0},
        @{@"title": HL(@"choice.high"), @"value": @1.6}
    ];
    NSMutableArray<NSMenuItem *> *sensitivityItems = [NSMutableArray array];
    for (NSDictionary *choice in sensitivityChoices) {
        NSMenuItem *item = [[NSMenuItem alloc]
            initWithTitle:choice[@"title"]
            action:@selector(setSensitivity:)
            keyEquivalent:@""];
        item.target = self;
        item.representedObject = choice[@"value"];
        [sensitivityMenu addItem:item];
        [sensitivityItems addObject:item];
    }
    _sensitivityMenuItems = sensitivityItems.copy;
    sensitivityRoot.submenu = sensitivityMenu;
    [menu addItem:sensitivityRoot];

    [menu addItem:NSMenuItem.separatorItem];
    _permissionMenuItem = [[NSMenuItem alloc]
        initWithTitle:HL(@"menu.request_permissions")
        action:@selector(requestPermissions:)
        keyEquivalent:@""];
    _permissionMenuItem.target = self;
    [menu addItem:_permissionMenuItem];

    NSMenuItem *quitItem = [[NSMenuItem alloc]
        initWithTitle:HL(@"menu.quit")
        action:@selector(terminate:)
        keyEquivalent:@"q"];
    quitItem.target = NSApp;
    [menu addItem:quitItem];
    _statusItem.menu = menu;
}

- (void)startSensorRequestingAccess:(BOOL)requestAccess {
    if (!haptik_sensor_available()) {
        _sensorStatus = HL(@"status.sensor.not_found");
        [self refreshMenu];
        haptik_debug_log("accelerometer device not found");
        return;
    }

    haptik_sensor_access_t access = haptik_sensor_check_access();
    if (access != HAPTIK_SENSOR_ACCESS_GRANTED && requestAccess) {
        if (!CGPreflightListenEventAccess()) {
            (void)CGRequestListenEventAccess();
        }
        (void)haptik_sensor_request_access();
        access = haptik_sensor_check_access();
    }
    if (access != HAPTIK_SENSOR_ACCESS_GRANTED) {
        _sensorStatus = access == HAPTIK_SENSOR_ACCESS_DENIED
            ? HL(@"status.sensor.permission_denied")
            : HL(@"status.sensor.permission_required");
        [self refreshMenu];
        haptik_debug_log("sensor access unavailable: listen=%d hid=%d",
            CGPreflightListenEventAccess(), (int)access);
        return;
    }

    haptik_sensor_destroy(_sensor);
    _sensor = haptik_sensor_create(sensor_sample_callback, (__bridge void *)self);
    if (_sensor == NULL) {
        _sensorStatus = HL(@"status.sensor.creation_failed");
        [self refreshMenu];
        return;
    }
    const haptik_sensor_result_t result = haptik_sensor_start(_sensor);
    if (result == HAPTIK_SENSOR_OK) {
        _sensorStatus = HL(@"status.sensor.waiting");
        _previousSampleCount = atomic_load(&_sensorSampleCount);
        _stalledChecks = 0;
        haptik_debug_log("accelerometer opened successfully");
    } else {
        _sensorStatus = [NSString stringWithFormat:
            HL(@"status.sensor.error.format"),
            haptik_sensor_result_string(result)];
        haptik_debug_log("accelerometer open failed: %s",
            haptik_sensor_result_string(result));
        haptik_sensor_destroy(_sensor);
        _sensor = NULL;
    }
    [self refreshMenu];
}

- (void)startKeyboardCapturePrompting:(BOOL)prompt {
    if (_eventTap != NULL) {
        _keyboardStatus = HL(@"status.keyboard.active");
        return;
    }

    if (prompt) {
        if (!CGPreflightListenEventAccess()) {
            (void)CGRequestListenEventAccess();
        }
        NSDictionary *options = @{(__bridge NSString *)kAXTrustedCheckOptionPrompt: @YES};
        (void)AXIsProcessTrustedWithOptions((__bridge CFDictionaryRef)options);
    }

    haptik_debug_log("keyboard capture attempt: listen=%d accessibility=%d",
        CGPreflightListenEventAccess(), AXIsProcessTrusted());

    const CGEventMask mask = CGEventMaskBit(kCGEventKeyDown);
    _eventTap = CGEventTapCreate(
        kCGHIDEventTap,
        kCGHeadInsertEventTap,
        kCGEventTapOptionListenOnly,
        mask,
        keyboard_event_callback,
        (__bridge void *)self
    );
    if (_eventTap == NULL) {
        _keyboardStatus = CGPreflightListenEventAccess()
            ? HL(@"status.keyboard.creation_failed")
            : HL(@"status.keyboard.permission_required");
        haptik_debug_log("CGEventTapCreate failed");

        if (_globalKeyMonitor == nil) {
            __weak HaptikAppDelegate *weakSelf = self;
            _globalKeyMonitor = [NSEvent
                addGlobalMonitorForEventsMatchingMask:NSEventMaskKeyDown
                handler:^(NSEvent *event) {
                    HaptikAppDelegate *strongSelf = weakSelf;
                    if (strongSelf == nil || !strongSelf->_enabled) {
                        return;
                    }
                    const float intensity = haptik_impact_intensity_at(
                        &strongSelf->_impactDetector,
                        monotonic_seconds()
                    );
                    haptik_audio_trigger(
                        strongSelf->_audio,
                        event.keyCode,
                        intensity
                    );
                    const uint_fast64_t count = atomic_fetch_add_explicit(
                        &strongSelf->_keyEventCount,
                        1,
                        memory_order_relaxed
                    ) + 1U;
                    if (count <= 3U) {
                        haptik_debug_log(
                            "NSEvent keyboard fallback received (count=%llu)",
                            (unsigned long long)count
                        );
                    }
                }];
            haptik_debug_log("NSEvent global fallback installed");
        }
        [self refreshMenu];
        return;
    }

    _eventTapSource = CFMachPortCreateRunLoopSource(
        kCFAllocatorDefault,
        _eventTap,
        0
    );
    CFRunLoopAddSource(
        CFRunLoopGetMain(),
        _eventTapSource,
        kCFRunLoopCommonModes
    );
    CGEventTapEnable(_eventTap, true);
    if (_globalKeyMonitor != nil) {
        [NSEvent removeMonitor:_globalKeyMonitor];
        _globalKeyMonitor = nil;
        haptik_debug_log("NSEvent fallback removed; CGEventTap is active");
    }
    _keyboardStatus = HL(@"status.keyboard.active");
    haptik_debug_log("CGEventTap keyboard capture active");
    [self refreshMenu];
}

- (void)retryPermissionsIfNeeded:(NSTimer *)timer {
    (void)timer;
    if (_eventTap == NULL && CGPreflightListenEventAccess()) {
        [self startKeyboardCapturePrompting:NO];
    }
    if (_sensor == NULL &&
        haptik_sensor_check_access() == HAPTIK_SENSOR_ACCESS_GRANTED) {
        [self startSensorRequestingAccess:NO];
    }
    const uint_fast64_t samples = atomic_load(&_sensorSampleCount);
    if (_sensor != NULL) {
        if (samples > _previousSampleCount) {
            _stalledChecks = 0;
            _sensorStatus = [NSString stringWithFormat:HL(@"status.sensor.active.format"),
                (unsigned long long)((samples - _previousSampleCount) / 2)];
        } else {
            _sensorStatus = HL(@"status.sensor.retrying");
            if (++_stalledChecks >= 3) {
                haptik_sensor_destroy(_sensor);
                _sensor = NULL;
                _stalledChecks = 0;
            }
        }
    }
    _previousSampleCount = samples;
    [self refreshMenu];
}

- (void)refreshMenu {
    if (_statusMenuItem == nil) {
        return;
    }
    _statusMenuItem.title = [NSString stringWithFormat:
        @"%@\n%@\n%@",
        _sensorStatus ?: HL(@"status.sensor.unknown"),
        _keyboardStatus ?: HL(@"status.keyboard.unknown"),
        _soundStatus ?: HL(@"status.sound.unknown")];
    _diagnosticsLabel.stringValue = [NSString stringWithFormat:
        HL(@"diagnostics.format"),
        _sensorStatus, _keyboardStatus, _soundStatus,
        (unsigned long long)atomic_load(&_keyEventCount),
        atomic_load(&_impactDetector.published_intensity) / 10000.0];
    _enabledMenuItem.state = _enabled ? NSControlStateValueOn : NSControlStateValueOff;
    _statusItem.button.title = _enabled ? @"⌨︎" : @"⌨︎×";

    for (NSMenuItem *item in _volumeMenuItems) {
        const float candidate = [item.representedObject floatValue];
        item.state = fabsf(candidate - _volume) < 0.02F
            ? NSControlStateValueOn
            : NSControlStateValueOff;
    }
    for (NSMenuItem *item in _sensitivityMenuItems) {
        const float candidate = [item.representedObject floatValue];
        item.state = fabsf(candidate - _sensitivity) < 0.02F
            ? NSControlStateValueOn
            : NSControlStateValueOff;
    }
    for (NSMenuItem *item in _soundMenuItems) {
        item.state = [item.representedObject isEqualToString:_selectedSoundPackID]
            ? NSControlStateValueOn
            : NSControlStateValueOff;
    }
}

- (NSArray<NSDictionary *> *)availableSoundPacks {
    return @[
        @{@"id": @"kailh_white", @"title": @"Kailh Box White"},
        @{@"id": @"alpaca", @"title": @"Durock Alpaca"},
        @{@"id": @"blackink", @"title": @"Gateron Black Ink"},
        @{@"id": @"bluealps", @"title": @"SKCM Blue Alps"},
        @{@"id": @"boxnavy", @"title": @"Kailh Box Navy"},
        @{@"id": @"buckling", @"title": @"IBM Buckling Spring"},
        @{@"id": @"cream", @"title": @"NovelKeys Cream"},
        @{@"id": @"holypanda", @"title": @"Holy Panda"},
        @{@"id": @"mxblack", @"title": @"Cherry MX Black"},
        @{@"id": @"mxblue", @"title": @"Cherry MX Blue"},
        @{@"id": @"mxbrown", @"title": @"Cherry MX Brown"},
        @{@"id": @"redink", @"title": @"Gateron Red Ink"},
        @{@"id": @"topre", @"title": @"Topre"},
        @{@"id": @"turquoise", @"title": @"Turquoise Tealios"}
    ];
}

- (BOOL)loadSoundPackID:(NSString *)packID startAudio:(BOOL)startAudio {
    NSDictionary *selected = nil;
    for (NSDictionary *pack in [self availableSoundPacks]) {
        if ([pack[@"id"] isEqualToString:packID]) {
            selected = pack;
            break;
        }
    }
    if (selected == nil || _audio == NULL) {
        return NO;
    }

    haptik_audio_stop(_audio);
    NSString *directory = nil;
    BOOL loaded = NO;
    if ([packID isEqualToString:@"kailh_white"]) {
        directory = [NSBundle.mainBundle.resourcePath
            stringByAppendingPathComponent:@"Sounds/KailhWhite"];
        loaded = haptik_audio_load_wav_pack(
            _audio,
            directory.fileSystemRepresentation
        );
    } else {
        directory = [[NSBundle.mainBundle.resourcePath
            stringByAppendingPathComponent:@"Sounds/KBSim"]
            stringByAppendingPathComponent:packID];
        loaded = haptik_audio_load_kbsim_pack(
            _audio,
            directory.fileSystemRepresentation
        );
    }

    if (loaded) {
        _selectedSoundPackID = packID.copy;
        _soundStatus = [NSString stringWithFormat:
            HL(@"status.sound.loaded.format"), selected[@"title"]];
        haptik_debug_log("sound pack loaded: %s", packID.UTF8String);
    } else {
        _soundStatus = [NSString stringWithFormat:
            HL(@"status.sound.error.format"), haptik_audio_last_error(_audio)];
        haptik_debug_log("sound pack load failed (%s): %s",
            packID.UTF8String, haptik_audio_last_error(_audio));
    }

    if (startAudio && !haptik_audio_start(_audio)) {
        _soundStatus = [NSString stringWithFormat:
            HL(@"status.audio.engine_error.format"), haptik_audio_last_error(_audio)];
        loaded = NO;
    }
    return loaded;
}

- (IBAction)toggleEnabled:(id)sender {
    (void)sender;
    _enabled = !_enabled;
    [NSUserDefaults.standardUserDefaults setBool:_enabled forKey:@"enabled"];
    [self refreshMenu];
}

- (IBAction)setVolume:(NSMenuItem *)sender {
    _volume = [sender.representedObject floatValue];
    haptik_audio_set_volume(_audio, _volume);
    [NSUserDefaults.standardUserDefaults setFloat:_volume forKey:@"volume"];
    haptik_audio_trigger(_audio, 36, 0.65F);
    [self refreshMenu];
}

- (IBAction)setSensitivity:(NSMenuItem *)sender {
    _sensitivity = [sender.representedObject floatValue];
    haptik_impact_set_sensitivity(&_impactDetector, _sensitivity);
    [NSUserDefaults.standardUserDefaults setFloat:_sensitivity forKey:@"sensitivity"];
    [self refreshMenu];
}

- (IBAction)selectSoundPack:(NSMenuItem *)sender {
    NSString *packID = sender.representedObject;
    if ([self loadSoundPackID:packID startAudio:YES]) {
        [NSUserDefaults.standardUserDefaults setObject:packID forKey:@"soundPack"];
        haptik_audio_set_volume(_audio, _volume);
        atomic_store_explicit(&_keyEventCount, 0, memory_order_relaxed);
        haptik_audio_trigger(_audio, 36, 0.65F);
    }
    [self refreshMenu];
}

- (void)systemDidWake:(NSNotification *)notification {
    (void)notification;
    haptik_sensor_destroy(_sensor);
    _sensor = NULL;
    [self startSensorRequestingAccess:NO];
}

- (BOOL)applicationShouldHandleReopen:(NSApplication *)sender hasVisibleWindows:(BOOL)visible {
    (void)sender;
    (void)visible;
    [self showWindow:nil];
    return YES;
}

- (IBAction)showWindow:(id)sender {
    (void)sender;
    if (_window == nil) {
        _window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 540, 330)
            styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable
            backing:NSBackingStoreBuffered defer:NO];
        _window.title = @"Haptik";
        _window.releasedWhenClosed = NO;
        [_window center];
        _diagnosticsLabel = [NSTextField wrappingLabelWithString:@""];
        _diagnosticsLabel.frame = NSMakeRect(24, 82, 492, 225);
        _diagnosticsLabel.font = [NSFont systemFontOfSize:14];
        [_window.contentView addSubview:_diagnosticsLabel];
        NSArray *titles = @[
            HL(@"button.check_permissions"),
            HL(@"button.input_monitoring"),
            HL(@"button.test_sound")
        ];
        SEL actions[] = {@selector(requestPermissions:), @selector(openInputSettings:), @selector(testSound:)};
        for (NSUInteger i = 0; i < titles.count; i++) {
            NSButton *button = [NSButton buttonWithTitle:titles[i] target:self action:actions[i]];
            button.frame = NSMakeRect(24 + i * 168, 24, 158, 32);
            [_window.contentView addSubview:button];
        }
    }
    [self refreshMenu];
    [_window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
}

- (IBAction)openInputSettings:(id)sender {
    (void)sender;
    [NSWorkspace.sharedWorkspace openURL:[NSURL URLWithString:
        @"x-apple.systempreferences:com.apple.preference.security?Privacy_ListenEvent"]];
}

- (IBAction)testSound:(id)sender {
    (void)sender;
    haptik_audio_trigger(_audio, 36, 0.65F);
}

- (IBAction)requestPermissions:(id)sender {
    (void)sender;
    [self startKeyboardCapturePrompting:YES];
    [self startSensorRequestingAccess:YES];
    [self refreshMenu];
}

@end

int main(int argc, const char *argv[]) {
    (void)argc;
    (void)argv;
    @autoreleasepool {
        NSApplication *application = NSApplication.sharedApplication;
        HaptikAppDelegate *delegate = [[HaptikAppDelegate alloc] init];
        application.delegate = delegate;
        [application run];
    }
    return 0;
}
