// Modern iOS platform interop for ClassiCube (iOS 16+, arm64)
// Based on src/ios/interop_ios.m, updated to use non-deprecated APIs.
#define GLES_SILENCE_DEPRECATION
#include "../Bitmap.h"
#include "../Input.h"
#include "../Platform.h"
#include "../String_.h"
#include "../Errors.h"
#include "../Drawer2D.h"
#include "../Launcher.h"
#include "../Funcs.h"
#include "../Gui.h"
#include "../Window.h"
#include "../Event.h"
#include "../Logger.h"
#include "../ExtMath.h"
#include <mach-o/dyld.h>
#include <sys/stat.h>
#import <UIKit/UIKit.h>
#import <UIKit/UIPasteboard.h>
#import <CoreText/CoreText.h>

// iOS 16 always has the modern text attribute names available
#define TEXT_ATTRIBUTE_FONT  NSFontAttributeName
#define TEXT_ATTRIBUTE_COLOR NSForegroundColorAttributeName

// shared state with Window_ios.m
extern UIViewController* cc_controller;

// The window scene the game's UIWindow attaches to. Modern iOS (apps built
//  against the iOS 13+ SDK) requires the UIScene lifecycle, so the scene
//  delegate below captures it for AllocWindow() in Window_ios.m to use.
UIWindowScene* cc_window_scene;

UIColor* ToUIColor(BitmapCol color, float A);
NSString* ToNSString(const cc_string* text);
UIInterfaceOrientationMask SupportedOrientations(void);
void LogUnhandledNSErrors(NSException* ex);


@interface CCSceneDelegate : NSObject<UIWindowSceneDelegate>
@end

@interface CCAppDelegate : UIResponder<UIApplicationDelegate>
@end

@implementation CCAppDelegate

#include "../main_impl.h"
- (void)runMainLoop {
    /* ClassiCube is sort of and sort of not the executable */
    /*  on iOS - UIKit is responsible for kickstarting the game. */
    SetupProgram(1, NULL);
    for (;;) { RunProgram(1, NULL); }
}

- (BOOL)application:(UIApplication *)application didFinishLaunchingWithOptions:(NSDictionary *)launchOptions {
    // The game loop is started from CCSceneDelegate once the window scene has
    //  connected (see below), so that a scene exists before the window is made.
    return YES;
}

- (UISceneConfiguration *)application:(UIApplication *)application configurationForConnectingSceneSession:(UISceneSession *)connectingSceneSession options:(UISceneConnectionOptions *)options {
    UISceneConfiguration* cfg = [UISceneConfiguration configurationWithName:@"Default Configuration" sessionRole:connectingSceneSession.role];
    cfg.delegateClass = [CCSceneDelegate class];
    return cfg;
}

- (void)applicationWillResignActive:(UIApplication *)application {
    Platform_LogConst("INACTIVE");
    Window_Main.Focused = false;
    Event_RaiseVoid(&WindowEvents.FocusChanged);
}

- (void)applicationDidEnterBackground:(UIApplication *)application {
    Platform_LogConst("BACKGROUND");
}

- (void)applicationWillEnterForeground:(UIApplication *)application {
    Platform_LogConst("FOREGROUND");
}

- (void)applicationDidBecomeActive:(UIApplication *)application {
    Platform_LogConst("ACTIVE");
    Window_Main.Focused = true;
    Event_RaiseVoid(&WindowEvents.FocusChanged);
}

- (void)applicationWillTerminate:(UIApplication *)application {
    // TODO implement somehow, prob need a variable in Program.c
}

- (UIInterfaceOrientationMask)application:(UIApplication *)application supportedInterfaceOrientationsForWindow:(UIWindow *)window {
    return SupportedOrientations();
}
@end


@implementation CCSceneDelegate
- (void)scene:(UIScene *)scene willConnectToSession:(UISceneSession *)session options:(UISceneConnectionOptions *)connectionOptions {
    // Remember the scene so AllocWindow() can attach the game's window to it
    if ([scene isKindOfClass:[UIWindowScene class]])
        cc_window_scene = (UIWindowScene*)scene;

    // Start the game now that a scene exists. Guard against the scene
    //  reconnecting (e.g. after returning from the background) starting it twice.
    static cc_bool launched;
    if (launched) return;
    launched = true;

    id appDelegate = [UIApplication sharedApplication].delegate;
    [appDelegate performSelector:@selector(runMainLoop) withObject:nil afterDelay:0.0];
}

- (void)sceneDidBecomeActive:(UIScene *)scene {
    Platform_LogConst("ACTIVE");
    Window_Main.Focused = true;
    Event_RaiseVoid(&WindowEvents.FocusChanged);
}

- (void)sceneWillResignActive:(UIScene *)scene {
    Platform_LogConst("INACTIVE");
    Window_Main.Focused = false;
    Event_RaiseVoid(&WindowEvents.FocusChanged);
}
@end


static void LogUnhandled(NSString* str) {
    if (!str) return;
    const char* src = [str UTF8String];
    if (!src) return;

    cc_string msg = String_FromReadonly(src);
    Platform_Log(msg.buffer, msg.length);
    Logger_Log(&msg);
}

void LogUnhandledNSErrors(NSException* ex) {
    // last chance to log exception details before process dies
    LogUnhandled(@"About to die from unhandled NSException..");
    LogUnhandled([ex name]);
    LogUnhandled([ex reason]);
}

int main(int argc, char * argv[]) {
    NSSetUncaughtExceptionHandler(LogUnhandledNSErrors);

    @autoreleasepool {
		NSString* className = NSStringFromClass([CCAppDelegate class]);
        return UIApplicationMain(argc, argv, nil, className);
    }
}


/*########################################################################################################################*
 *------------------------------------------------------Common helpers--------------------------------------------------------*
 *#########################################################################################################################*/
UIColor* ToUIColor(BitmapCol color, float A) {
    float R = BitmapCol_R(color) / 255.0f;
    float G = BitmapCol_G(color) / 255.0f;
    float B = BitmapCol_B(color) / 255.0f;
    return [UIColor colorWithRed:R green:G blue:B alpha:A];
}

NSString* ToNSString(const cc_string* text) {
    char raw[NATIVE_STR_LEN];
    String_EncodeUtf8(raw, text);
    return [NSString stringWithUTF8String:raw];
}


/*########################################################################################################################*
 *------------------------------------------------------Logging/Time-------------------------------------------------------*
 *#########################################################################################################################*/
void Platform_Log(const char* msg, int len) {
    char tmp[2048 + 1];
    len = min(len, 2048);

    Mem_Copy(tmp, msg, len); tmp[len] = '\0';
    NSLog(@"%s", tmp);
}


/*########################################################################################################################*
 *--------------------------------------------------------Updater----------------------------------------------------------*
 *#########################################################################################################################*/
const struct UpdaterInfo Updater_Info = {
    "&eRedownload and reinstall to update", 0
};
cc_bool Updater_Clean(void) { return true; }

cc_result Updater_GetBuildTime(cc_uint64* t) {
    char path[NATIVE_STR_LEN + 1] = { 0 };
    uint32_t size = NATIVE_STR_LEN;
    if (_NSGetExecutablePath(path, &size)) return ERR_INVALID_ARGUMENT;

    struct stat sb;
    if (stat(path, &sb) == -1) return errno;
    *t = (cc_uint64)sb.st_mtime;
    return 0;
}

cc_result Updater_Start(const char** action)   { *action = "Updating game"; return ERR_NOT_SUPPORTED; }
cc_result Updater_MarkExecutable(void)         { return 0; }
cc_result Updater_SetNewBuildTime(cc_uint64 t) { return ERR_NOT_SUPPORTED; }


/*########################################################################################################################*
 *--------------------------------------------------------Platform--------------------------------------------------------*
 *#########################################################################################################################*/
cc_result Process_StartOpen(const cc_string* args) {
    // openURL:options:completionHandler: - iOS 10 (replaces deprecated openURL:)
	NSString* str = ToNSString(args);
    NSURL* url    = [[NSURL alloc] initWithString:str];
    if (!url) return ERR_INVALID_ARGUMENT;

    UIApplication* app = [UIApplication sharedApplication];
    [app openURL:url options:@{} completionHandler:nil];

	[url autorelease];
    return 0;
}

cc_result Platform_SetDefaultCurrentDirectory(int argc, char **argv) {
    NSArray* array = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES);
    if ([array count] <= 0) return ERR_NOT_SUPPORTED;

    NSString* str    = [array objectAtIndex:0];
    const char* path = [str fileSystemRepresentation];

    mkdir(path, S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH);
    return chdir(path) == -1 ? errno : 0;
}

void Platform_ShareScreenshot(const cc_string* filename) {
    cc_string path; char pathBuffer[FILENAME_SIZE];
    String_InitArray(path, pathBuffer);
    String_Format1(&path, "screenshots/%s", filename);

    NSString* pathStr = ToNSString(&path);
    UIImage* img = [UIImage imageWithContentsOfFile:pathStr];

    UIActivityViewController* act;
    act = [UIActivityViewController alloc];
    act = [act initWithActivityItems:@[ @"Share screenshot via", img] applicationActivities:Nil];

    // iPad requires a popover anchor, otherwise presenting crashes
    act.popoverPresentationController.sourceView = cc_controller.view;
    act.popoverPresentationController.sourceRect = CGRectMake(cc_controller.view.bounds.size.width / 2,
                                                              cc_controller.view.bounds.size.height / 2, 0, 0);
    [cc_controller presentViewController:act animated:true completion:Nil];

	[act autorelease];
}

void GetDeviceUUID(cc_string* str) {
    UIDevice* device = [UIDevice currentDevice];
    NSUUID*   uuid   = [device identifierForVendor];
    NSString* string = [uuid UUIDString];

    const char* src = [string UTF8String];
    String_AppendUtf8(str, src, String_Length(src));
}

void Directory_GetCachePath(cc_string* path) {
    NSArray* array = NSSearchPathForDirectoriesInDomains(NSCachesDirectory, NSUserDomainMask, YES);
    if ([array count] <= 0) return;

    NSString* str    = [array objectAtIndex:0];
    const char* utf8 = [str UTF8String];

    String_AppendUtf8(path, utf8, String_Length(utf8));
}


/*########################################################################################################################*
 *-----------------------------------------------------Font handling-------------------------------------------------------*
 *#########################################################################################################################*/
#ifndef CC_BUILD_FREETYPE
void interop_GetFontNames(struct StringsBuffer* buffer) {
    NSArray* families = [UIFont familyNames];
	[families retain];

    char tmpBuffer[NATIVE_STR_LEN];
    cc_string tmp = String_FromArray(tmpBuffer);

    for (NSString* family in families)
    {
        const char* str = [family UTF8String];
        String_AppendUtf8(&tmp, str, String_Length(str));
        StringsBuffer_Add(buffer, &tmp);
        tmp.length = 0;
    }

    StringsBuffer_Sort(buffer);
	[families autorelease];
}

static void InitFont(struct FontDesc* desc, UIFont* font) {
	desc->handle = [font retain];
	desc->height = Math_Ceil(Math_AbsF(font.ascender) + Math_AbsF(font.descender));
}

static UIFont* TryCreateBoldFont(NSString* name, CGFloat uiSize) {
    NSArray* fontNames = [UIFont fontNamesForFamilyName:name];
    for (NSString* fontName in fontNames)
    {
		NSRange boldRange = [fontName rangeOfString:@"Bold" options:NSCaseInsensitiveSearch];
        if (boldRange.location != NSNotFound)
            return [UIFont fontWithName:fontName size:uiSize];
    }
    return nil;
}

cc_result interop_SysFontMake(struct FontDesc* desc, const cc_string* fontName, int size, int flags) {
    CGFloat uiSize = size * 96.0f / 72.0f; // convert from point size
    NSString* name = ToNSString(fontName);
    UIFont* font   = (flags & FONT_FLAGS_BOLD) ? TryCreateBoldFont(name, uiSize) : nil;

    if (!font) font = [UIFont fontWithName:name size:uiSize];
    if (!font) return ERR_NOT_SUPPORTED;

    InitFont(desc, font);
    return 0;
}

void interop_SysMakeDefault(struct FontDesc* desc, int size, int flags) {
    CGFloat uiSize = size * 96.0f / 72.0f; // convert from point size
    UIFont* font;

    if (flags & FONT_FLAGS_BOLD) {
        font = [UIFont boldSystemFontOfSize:uiSize];
    } else {
        font = [UIFont systemFontOfSize:uiSize];
    }
    InitFont(desc, font);
}

void interop_SysFontFree(void* handle) {
	UIFont* font = (UIFont*)handle;
	[font autorelease];
}

static NSMutableAttributedString* CreateAttributedString(struct DrawTextArgs* args, cc_bool shadow) {
    UIFont* font   = (UIFont*)args->font->handle;
    cc_string left = args->text, part;
    char colorCode = 'f';
    NSMutableAttributedString* str = [[NSMutableAttributedString alloc] init];

    while (Drawer2D_UNSAFE_NextPart(&left, &part, &colorCode))
    {
        BitmapCol color   = Drawer2D_GetColor(colorCode);
        if (shadow) color = GetShadowColor(color);

        NSString* bit = ToNSString(&part);
        NSRange range = NSMakeRange(str.length, bit.length);

        NSMutableString* dst = [str mutableString];
        [dst appendString:bit];

        [str addAttribute:TEXT_ATTRIBUTE_FONT  value:font                   range:range];
        [str addAttribute:TEXT_ATTRIBUTE_COLOR value:ToUIColor(color, 1.0f) range:range];

        if (args->font->flags & FONT_FLAGS_UNDERLINE) {
            NSNumber* style = [NSNumber numberWithInt:kCTUnderlineStyleSingle];
            [str addAttribute:NSUnderlineStyleAttributeName value:style range:range];
        }
    }
    return str;
}

int interop_SysTextWidth(struct DrawTextArgs* args) {
    NSMutableAttributedString* str = CreateAttributedString(args, false);

    CTLineRef line = CTLineCreateWithAttributedString((CFAttributedStringRef)str);
    CGRect bounds  = CTLineGetImageBounds(line, NULL);

    CGFloat ascent, descent, leading;
    double width = CTLineGetTypographicBounds(line, &ascent, &descent, &leading);

    CFRelease(line);
	[str release];
    return Math_Ceil(width);
}

void interop_SysTextDraw(struct DrawTextArgs* args, struct Context2D* ctx, int x, int y, cc_bool shadow) {
    UIFont* font = (UIFont*)args->font->handle;
    NSMutableAttributedString* str = CreateAttributedString(args, shadow);

    float X = x, Y = y;
    if (shadow) { X += 1.3f; Y -= 1.3f; }

    CTLineRef line = CTLineCreateWithAttributedString((CFAttributedStringRef)str);
    struct Bitmap* bmp = &ctx->bmp;

    CGContextRef cg_ctx = CGBitmapContextCreate(bmp->scan0, bmp->width, ctx->height, 8, bmp->width * 4,
                                                CGColorSpaceCreateDeviceRGB(), kCGBitmapByteOrder32Host | kCGImageAlphaNoneSkipFirst);
    CGContextSetTextPosition(cg_ctx, X, Y - font.descender);
    CTLineDraw(line, cg_ctx);
    CGContextRelease(cg_ctx);

    CFRelease(line);
	[str release];
}
#endif
