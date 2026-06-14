// Modern iOS window backend for ClassiCube
// Targets iOS 16+, arm64 only. Unlike src/ios/Window_ios.m (which preserves
//  compatibility back to iOS 6), this version assumes a modern iPhone/iPad and
//  uses up to date APIs:
//   - Native Retina rendering for the 3D game (full physical pixel resolution)
//   - UIAlertController instead of the removed UIAlertView
//   - UTType based document picker instead of deprecated document type strings
//   - UIPasteboard backed clipboard (copy/paste actually works)
//   - GameController framework for MFi / Xbox / PlayStation controllers
//   - Taptic Engine haptic feedback
//   - Modern (iOS 16) orientation locking via window scene geometry requests
#define GLES_SILENCE_DEPRECATION
#include "../_WindowBase.h"
#include "../Bitmap.h"
#include "../Input.h"
#include "../Platform.h"
#include "../String_.h"
#include "../Errors.h"
#include "../Drawer2D.h"
#include "../Launcher.h"
#include "../Funcs.h"
#include "../Gui.h"
#include "../ExtMath.h"
#include <mach-o/dyld.h>
#include <sys/stat.h>
#import <UIKit/UIKit.h>
#import <UIKit/UIPasteboard.h>
#import <CoreText/CoreText.h>
#import <GameController/GameController.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

// shared state with LBackend_ios.m and interop_ios.m
CGContextRef win_ctx;
UIView* view_handle;
UIViewController* cc_controller;

UIColor* ToUIColor(BitmapCol color, float A);
NSString* ToNSString(const cc_string* text);
void LInput_SetKeyboardType(UITextField* fld, int flags);
void LInput_SetPlaceholder(UITextField* fld, const char* placeholder);
UIInterfaceOrientationMask SupportedOrientations(void);
void LogUnhandledNSErrors(NSException* ex);
void IOSHaptic_Light(void);

// Physical pixels per point on this display (the Retina factor).
//  The 3D game renders at full physical resolution, whereas the 2D launcher
//  (which uses native UIKit widgets laid out in points) stays in point space.
static CGFloat display_scale = 1.0f;
static CGFloat CurrentTouchScale(void) {
    return Window_Main.Is3D ? display_scale : 1.0f;
}


static UITextField* kb_widget;
void Window_SetKBWidget(UITextField* widget) {
	if (kb_widget) [kb_widget autorelease];

	if (widget) [widget retain];
	kb_widget = widget;
}

@interface CCWindow : UIWindow
@end

@interface CCViewController : UIViewController<UIDocumentPickerDelegate>
@end
static UIWindow* win_handle;

static void AddTouch(UITouch* t) {
    CGFloat s = CurrentTouchScale();
    CGPoint loc = [t locationInView:view_handle];
    Input_AddTouch((long)t, loc.x * s, loc.y * s);
}

static void UpdateTouch(UITouch* t) {
    CGFloat s = CurrentTouchScale();
    CGPoint loc = [t locationInView:view_handle];
    Input_UpdateTouch((long)t, loc.x * s, loc.y * s);
}

static void RemoveTouch(UITouch* t) {
    CGFloat s = CurrentTouchScale();
    CGPoint loc = [t locationInView:view_handle];
    Input_RemoveTouch((long)t, loc.x * s, loc.y * s);
}

static cc_bool landscape_locked;
UIInterfaceOrientationMask SupportedOrientations(void) {
    if (landscape_locked)
        return UIInterfaceOrientationMaskLandscape;
    return UIInterfaceOrientationMaskAll;
}

static cc_bool fullscreen = true;
static void UpdateStatusBar(void) {
    [cc_controller setNeedsStatusBarAppearanceUpdate];
}

static CGRect GetViewFrame(void) {
    // applicationFrame is removed on modern iOS, the game is always fullscreen
    return [UIScreen mainScreen].bounds;
}

@implementation CCWindow

- (void)touchesBegan:(NSSet*)touches withEvent:(UIEvent *)event {
    for (UITouch* t in touches) AddTouch(t);

    // clicking on the background should dismiss onscreen keyboard
    if (!Window_Main.Is3D) { [view_handle endEditing:NO]; }
}

- (void)touchesMoved:(NSSet*)touches withEvent:(UIEvent *)event {
    for (UITouch* t in touches) UpdateTouch(t);
}

- (void)touchesEnded:(NSSet*)touches withEvent:(UIEvent *)event {
    for (UITouch* t in touches) RemoveTouch(t);
}

- (void)touchesCancelled:(NSSet*)touches withEvent:(UIEvent *)event {
    for (UITouch* t in touches) RemoveTouch(t);
}

- (BOOL)isOpaque { return YES; }
@end


@implementation CCViewController
- (UIInterfaceOrientationMask)supportedInterfaceOrientations {
    return SupportedOrientations();
}

- (BOOL)shouldAutorotate {
    return YES;
}

- (void)viewDidLayoutSubviews {
	[super viewDidLayoutSubviews];

	CGRect frame = [view_handle frame];
	CGFloat scale = Window_Main.Is3D ? display_scale : 1.0f;
	int width  = (int)(frame.size.width  * scale);
	int height = (int)(frame.size.height * scale);
	if (width == Window_Main.Width && height == Window_Main.Height) return;

	Window_Main.Width  = width;
	Window_Main.Height = height;
	Event_RaiseVoid(&WindowEvents.Resized);
}

// ==== UIDocumentPickerDelegate ====
static FileDialogCallback open_dlg_callback;
static char save_buffer[FILENAME_SIZE];
static cc_string save_path = String_FromArray(save_buffer);

static void DeleteExportTempFile(void) {
    if (!save_path.length) return;

    char path[NATIVE_STR_LEN];
    String_EncodeUtf8(path, &save_path);
    unlink(path);
    save_path.length = 0;
}

- (void)documentPicker:(UIDocumentPickerViewController *)controller didPickDocumentsAtURLs:(NSArray<NSURL*>*)urls {
    if (urls.count == 0) { DeleteExportTempFile(); return; }
    NSURL* url       = urls[0];
    cc_bool scoped   = [url startAccessingSecurityScopedResource];
    const char* utf8 = url.path.UTF8String;

    char tmpBuffer[NATIVE_STR_LEN];
    cc_string tmp = String_FromArray(tmpBuffer);
    String_AppendUtf8(&tmp, utf8, String_Length(utf8));

    if (open_dlg_callback) {
        open_dlg_callback(&tmp);
        open_dlg_callback = NULL;
    }
    if (scoped) [url stopAccessingSecurityScopedResource];
    DeleteExportTempFile();
}

- (void)documentPickerWasCancelled:(UIDocumentPickerViewController *)controller {
    open_dlg_callback = NULL;
    DeleteExportTempFile();
}

static cc_bool kb_active;
- (void)keyboardDidShow:(NSNotification*)notification {
    NSDictionary* info = [notification userInfo];
    if (kb_active) return;
    kb_active = true;

    double interval   = [[info objectForKey:UIKeyboardAnimationDurationUserInfoKey] doubleValue];
    NSInteger curve   = [[info objectForKey:UIKeyboardAnimationCurveUserInfoKey] integerValue];
    CGRect kbFrame    = [[info objectForKey:UIKeyboardFrameEndUserInfoKey] CGRectValue];
    CGRect winFrame   = [view_handle frame];

    cc_bool can_shift = true;
    // would the active input widget be pushed offscreen?
    if (kb_widget) {
        CGRect curFrame = [kb_widget frame];
        can_shift = curFrame.origin.y > kbFrame.size.height;
    }
    if (can_shift) winFrame.origin.y = -kbFrame.size.height;
    Window_SetKBWidget(nil);

    [UIView animateWithDuration:interval delay: 0.0 options:curve animations:^{
        [view_handle setFrame:winFrame];
    } completion:nil];
}

- (void)keyboardDidHide:(NSNotification*)notification {
    NSDictionary* info = [notification userInfo];
    if (!kb_active) return;
    kb_active = false;
	Window_SetKBWidget(nil);

    double interval   = [[info objectForKey:UIKeyboardAnimationDurationUserInfoKey] doubleValue];
    NSInteger curve   = [[info objectForKey:UIKeyboardAnimationCurveUserInfoKey] integerValue];
    CGRect winFrame   = [view_handle frame];
    winFrame.origin.y = 0;

    [UIView animateWithDuration:interval delay: 0.0 options:curve animations:^{
       [view_handle setFrame:winFrame];
    } completion:nil];
}

- (BOOL)prefersStatusBarHidden {
    return fullscreen;
}

- (BOOL)prefersHomeIndicatorAutoHidden {
    // Hide the bottom home indicator while playing for a more immersive view
    return Window_Main.Is3D;
}

- (UIRectEdge)preferredScreenEdgesDeferringSystemGestures {
    // recent iOS versions have a 'bottom home bar', which when swiped up,
    //  switches out of ClassiCube and to the app list menu
    // overriding this forces the user to swipe up twice, which significantly
    //  reduces the chance of accidentally triggering this gesture
    return UIRectEdgeBottom;
}
@end


/*########################################################################################################################*
*--------------------------------------------------------Clipboard--------------------------------------------------------*
*#########################################################################################################################*/
void Clipboard_GetText(cc_string* value) {
    UIPasteboard* pb = [UIPasteboard generalPasteboard];
    NSString* str    = pb.string;
    if (!str) return;

    const char* utf8 = str.UTF8String;
    if (utf8) String_AppendUtf8(value, utf8, String_Length(utf8));
}

void Clipboard_SetText(const cc_string* value) {
    [UIPasteboard generalPasteboard].string = ToNSString(value);
}


/*########################################################################################################################*
*---------------------------------------------------------Haptics---------------------------------------------------------*
*#########################################################################################################################*/
static UIImpactFeedbackGenerator* haptic_gen;
void IOSHaptic_Light(void) {
    if (!haptic_gen) {
        haptic_gen = [[UIImpactFeedbackGenerator alloc] initWithStyle:UIImpactFeedbackStyleLight];
    }
    [haptic_gen impactOccurred];
}


/*########################################################################################################################*
*---------------------------------------------------------Window----------------------------------------------------------*
*#########################################################################################################################*/
// no cursor on iOS
void Cursor_GetRawPos(int* x, int* y) { *x = 0; *y = 0; }
void Cursor_SetPosition(int x, int y) { }
void Cursor_DoSetVisible(cc_bool visible) { }

void Window_SetTitle(const cc_string* title) {
    // not applicable on iOS
}

void Window_PreInit(void) {
    DisplayInfo.CursorVisible = true;
}

void Window_Init(void) {
    // keyboard now shifts the view up rather than resizing it
    Window_Main.SoftKeyboard = SOFT_KEYBOARD_SHIFT;
    Input_SetTouchMode(true);
    Input.Sources = INPUT_SOURCE_NORMAL;
    Gui_SetTouchUI(true);

    display_scale      = [UIScreen mainScreen].nativeScale;
    DisplayInfo.Depth  = 32;
    // Launcher (2D) defaults to point space, the 3D game switches to Retina pixels
    DisplayInfo.ScaleX = 1.0f;
    DisplayInfo.ScaleY = 1.0f;
    NSSetUncaughtExceptionHandler(LogUnhandledNSErrors);
}

void Window_Free(void) { }
void Window_SetSize(int width, int height) { }

void Window_Show(void) {
    [win_handle makeKeyAndVisible];
}

void Window_RequestClose(void) {
    Event_RaiseVoid(&WindowEvents.Closing);
}

void Window_ProcessEvents(float delta) {
    SInt32 res;
    // manually tick event queue
    do {
        res = CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0, TRUE);
    } while (res == kCFRunLoopRunHandledSource);
}


/*########################################################################################################################*
*--------------------------------------------------------Gamepads---------------------------------------------------------*
*#########################################################################################################################*/
void Gamepads_PreInit(void) { }
void Gamepads_Init(void)    { }

static void ProcessGamepadButtons(int port, GCExtendedGamepad* gp) {
    Gamepad_SetButton(port, CCPAD_1, gp.buttonA.isPressed);
    Gamepad_SetButton(port, CCPAD_2, gp.buttonB.isPressed);
    Gamepad_SetButton(port, CCPAD_3, gp.buttonX.isPressed);
    Gamepad_SetButton(port, CCPAD_4, gp.buttonY.isPressed);

    Gamepad_SetButton(port, CCPAD_L,  gp.leftShoulder.isPressed);
    Gamepad_SetButton(port, CCPAD_R,  gp.rightShoulder.isPressed);
    Gamepad_SetButton(port, CCPAD_ZL, gp.leftTrigger.isPressed);
    Gamepad_SetButton(port, CCPAD_ZR, gp.rightTrigger.isPressed);

    // buttonOptions/thumbstickButton may be nil on some controllers,
    //  sending a message to nil safely returns NO so no guard is needed
    Gamepad_SetButton(port, CCPAD_START,  gp.buttonMenu.isPressed);
    Gamepad_SetButton(port, CCPAD_SELECT, gp.buttonOptions.isPressed);
    Gamepad_SetButton(port, CCPAD_LSTICK, gp.leftThumbstickButton.isPressed);
    Gamepad_SetButton(port, CCPAD_RSTICK, gp.rightThumbstickButton.isPressed);

    Gamepad_SetButton(port, CCPAD_UP,    gp.dpad.up.isPressed);
    Gamepad_SetButton(port, CCPAD_DOWN,  gp.dpad.down.isPressed);
    Gamepad_SetButton(port, CCPAD_LEFT,  gp.dpad.left.isPressed);
    Gamepad_SetButton(port, CCPAD_RIGHT, gp.dpad.right.isPressed);
}

static void ProcessGamepadStick(int port, int axis, GCControllerDirectionPad* stick, float delta) {
    float x = stick.xAxis.value;
    float y = stick.yAxis.value;

    // small deadzone to ignore stick drift on worn controllers
    if (Math_AbsF(x) <= 0.1f) x = 0;
    if (Math_AbsF(y) <= 0.1f) y = 0;

    // GameController reports +Y as up, which already matches what the game expects
    Gamepad_SetAxis(port, axis, x, y, delta);
}

void Gamepads_Process(float delta) {
    int i = 0;
    for (GCController* controller in [GCController controllers])
    {
        if (i >= INPUT_MAX_GAMEPADS) break;
        GCExtendedGamepad* gp = controller.extendedGamepad;
        if (!gp) continue;

        int port = Gamepad_Connect(0x1C0 + i, PadBind_Defaults);
        ProcessGamepadButtons(port, gp);
        ProcessGamepadStick(port, PAD_AXIS_LEFT,  gp.leftThumbstick,  delta);
        ProcessGamepadStick(port, PAD_AXIS_RIGHT, gp.rightThumbstick, delta);
        i++;
    }
}


/*########################################################################################################################*
*----------------------------------------------------Onscreen keyboard----------------------------------------------------*
*#########################################################################################################################*/
@interface CCKBController : NSObject<UITextFieldDelegate>
@end

@implementation CCKBController
- (void)handleTextChanged:(id)sender {
    UITextField* src = (UITextField*)sender;
    NSString* text   = [src text];
    const char* str  = [text UTF8String];

    char tmpBuffer[NATIVE_STR_LEN];
    cc_string tmp = String_FromArray(tmpBuffer);
    String_AppendUtf8(&tmp, str, String_Length(str));

    Event_RaiseString(&InputEvents.TextChanged, &tmp);
}

// === UITextFieldDelegate ===
- (BOOL)textFieldShouldReturn:(UITextField *)textField {
    Input_SetPressed(CCKEY_ENTER);
    Input_SetReleased(CCKEY_ENTER);
    return YES;
}
@end

static UITextField* text_input;
static CCKBController* kb_controller;

void OnscreenKeyboard_Open(struct OpenKeyboardArgs* args) {
    if (!kb_controller) {
        kb_controller = [[CCKBController alloc] init];
    }
    DisplayInfo.ShowingSoftKeyboard = true;

    text_input = [[UITextField alloc] initWithFrame:CGRectZero];
    [text_input setHidden:YES];
    [text_input setDelegate:kb_controller];
    [text_input addTarget:kb_controller action:@selector(handleTextChanged:) forControlEvents:UIControlEventEditingChanged];

    LInput_SetKeyboardType(text_input, args->type);
    LInput_SetPlaceholder(text_input,  args->placeholder);

    [view_handle addSubview:text_input];
    [text_input becomeFirstResponder];

	[text_input autorelease];
}

void OnscreenKeyboard_SetText(const cc_string* text) {
    NSString* str = ToNSString(text);
    NSString* cur = [text_input text];

    if (cur && [str isEqualToString:cur]) return;
    [text_input setText:str];
}

void OnscreenKeyboard_Close(void) {
    DisplayInfo.ShowingSoftKeyboard = false;
    [text_input resignFirstResponder];
}


/*########################################################################################################################*
*--------------------------------------------------------Fullscreen-------------------------------------------------------*
*#########################################################################################################################*/
int Window_GetWindowState(void) {
    return fullscreen ? WINDOW_STATE_FULLSCREEN : WINDOW_STATE_NORMAL;
}

static void ToggleFullscreen(cc_bool isFullscreen) {
    fullscreen = isFullscreen;
    UpdateStatusBar();

    CGRect frame = GetViewFrame();
    [view_handle setFrame:frame];
}

cc_result Window_EnterFullscreen(void) {
    ToggleFullscreen(true); return 0;
}
cc_result Window_ExitFullscreen(void) {
    ToggleFullscreen(false); return 0;
}
int Window_IsObscured(void) { return 0; }

void Window_EnableRawMouse(void)  { DefaultEnableRawMouse(); }
void Window_UpdateRawMouse(void)  { }
void Window_DisableRawMouse(void) { DefaultDisableRawMouse(); }

void Window_LockLandscapeOrientation(cc_bool lock) {
    landscape_locked = lock;

    if (@available(iOS 16.0, *)) {
        [cc_controller setNeedsUpdateOfSupportedInterfaceOrientations];

        UIWindowScene* scene = win_handle.windowScene;
        if (scene) {
            UIInterfaceOrientationMask mask = lock ? UIInterfaceOrientationMaskLandscape : UIInterfaceOrientationMaskAll;
            UIWindowSceneGeometryPreferencesIOS* prefs =
                [[UIWindowSceneGeometryPreferencesIOS alloc] initWithInterfaceOrientations:mask];
            [scene requestGeometryUpdateWithPreferences:prefs errorHandler:^(NSError* error) { }];
            [prefs release];
        }
    }
}


/*#########################################################################################################################*
 *-----------------------------------------------------Window creation-----------------------------------------------------*
 *#########################################################################################################################*/
@interface CC3DView : UIView
@end
static void Init3DLayer(void);

static UIColor* CalcBackgroundColor(void) {
	// default to red if no themed background color yet
	if (!Launcher_Theme.BackgroundColor)
		return [UIColor redColor];

	return ToUIColor(Launcher_Theme.BackgroundColor, 1.0f);
}

static void AllocWindow(void) {
	if (cc_controller) return;
	cc_controller = [CCViewController alloc];

	CGRect bounds = GetViewFrame();
	win_handle    = [[CCWindow alloc] initWithFrame:bounds];
	[win_handle setRootViewController:cc_controller];

	Window_Main.Exists   = true;
	Window_Main.UIScaleX = DEFAULT_UI_SCALE_X;
	Window_Main.UIScaleY = DEFAULT_UI_SCALE_Y;

	Window_Main.Width  = bounds.size.width;
	Window_Main.Height = bounds.size.height;
	Window_Main.SoftKeyboardInstant = true;

	NSNotificationCenter* notifications = [NSNotificationCenter defaultCenter];
	[notifications addObserver:cc_controller selector:@selector(keyboardDidShow:) name:UIKeyboardWillShowNotification object:nil];
	[notifications addObserver:cc_controller selector:@selector(keyboardDidHide:) name:UIKeyboardWillHideNotification object:nil];
}

static void DoCreateWindow(void) {
	AllocWindow();
	UpdateStatusBar();

	UIColor* color = CalcBackgroundColor();
	[win_handle setBackgroundColor:color];
}

static void SetRootView(UIView* view) {
	view_handle = view;
	[view setMultipleTouchEnabled:YES];
	[cc_controller setView:view];

	// Required, otherwise view doesn't rotate with device anymore after going in-game
	[win_handle setRootViewController:nil];
	[win_handle setRootViewController:cc_controller];
}

void Window_Create2D(int width, int height) {
    Window_Main.Is3D = false;
    // Launcher uses native UIKit widgets laid out in points
    DisplayInfo.ScaleX = 1.0f;
    DisplayInfo.ScaleY = 1.0f;
    DoCreateWindow();

    UIView* view = [[UIView alloc] initWithFrame:CGRectZero];
	SetRootView(view);

	[view autorelease];
}

void Window_Create3D(int width, int height) {
    Window_Main.Is3D = true;
    // Game renders at full native Retina resolution; scale the UI to match
    DisplayInfo.ScaleX = display_scale;
    DisplayInfo.ScaleY = display_scale;
    DoCreateWindow();

    CC3DView* view = [[CC3DView alloc] initWithFrame:CGRectZero];
	SetRootView(view);
    [view setContentScaleFactor:display_scale];
    Init3DLayer();

	[view autorelease];
}

void Window_Destroy(void) { }


/*########################################################################################################################*
 *--------------------------------------------------------Dialogs---------------------------------------------------------*
 *#########################################################################################################################*/
static int alert_completed;
void ShowDialogCore(const char* title, const char* msg) {
	Platform_LogConst(title);
	Platform_LogConst(msg);
	NSString* _title = [NSString stringWithCString:title encoding:NSASCIIStringEncoding];
	NSString* _msg   = [NSString stringWithCString:msg encoding:NSASCIIStringEncoding];
	alert_completed  = false;

	UIAlertController* alert = [UIAlertController alertControllerWithTitle:_title message:_msg preferredStyle:UIAlertControllerStyleAlert];
	UIAlertAction* okBtn     = [UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleDefault handler:^(UIAlertAction* act) { alert_completed = true; }];
	[alert addAction:okBtn];
	[cc_controller presentViewController:alert animated:YES completion:nil];

	// loop until alert is closed
	while (!alert_completed) {
		Window_ProcessEvents(0.0);
		Thread_Sleep(16);
	}
}

static UTType* UTTypeForExtension(NSString* fileExt) {
    // fileExt is like ".cw" - strip the leading dot
    if ([fileExt hasPrefix:@"."]) fileExt = [fileExt substringFromIndex:1];
    if ([fileExt isEqualToString:@"zip"]) return UTTypeZIP;

    UTType* type = [UTType typeWithFilenameExtension:fileExt];
    return type ? type : UTTypeData;
}

cc_result Window_OpenFileDialog(const struct OpenFileDialogArgs* args) {
	NSMutableArray<UTType*>* types = [NSMutableArray array];
	const char* const* filters = args->filters;

	for (int i = 0; filters[i]; i++)
	{
		NSString* fileExt = [NSString stringWithUTF8String:filters[i]];
		UTType* type      = UTTypeForExtension(fileExt);
		if (type) [types addObject:type];
	}
	if (types.count == 0) [types addObject:UTTypeData];

	UIDocumentPickerViewController* dlg =
		[[UIDocumentPickerViewController alloc] initForOpeningContentTypes:types];

	open_dlg_callback = args->Callback;
	[dlg setDelegate:cc_controller];
	[dlg setAllowsMultipleSelection:NO];
	[cc_controller presentViewController:dlg animated:YES completion:nil];

	[dlg autorelease];
	return 0;
}

cc_result Window_SaveFileDialog(const struct SaveFileDialogArgs* args) {
	if (!args->defaultName.length) return SFD_ERR_NEED_DEFAULT_NAME;

	// save the item to a temp file, which is then (usually) later deleted by picker callbacks
	Directory_Create2(FILEPATH_RAW("Exported"));

	save_path.length = 0;
	String_Format2(&save_path, "Exported/%s%c", &args->defaultName, args->filters[0]);
	args->Callback(&save_path);

	NSString* str = ToNSString(&save_path);
	NSURL* url    = [NSURL fileURLWithPath:str isDirectory:NO];

	UIDocumentPickerViewController* dlg =
		[[UIDocumentPickerViewController alloc] initForExportingURLs:@[url]];

	[dlg setDelegate:cc_controller];
	[cc_controller presentViewController:dlg animated:YES completion:nil];

	[dlg autorelease];
	return 0;
}


/*########################################################################################################################*
*--------------------------------------------------------GLContext--------------------------------------------------------*
*#########################################################################################################################*/
#if CC_GFX_BACKEND_IS_GL()
#include <OpenGLES/ES2/gl.h>
#include <OpenGLES/ES2/glext.h>

static EAGLContext* ctx_handle;
static GLuint framebuffer;
static GLuint color_renderbuffer, depth_renderbuffer;
static int fb_width, fb_height;

static void UpdateColorbuffer(void) {
    CAEAGLLayer* layer = (CAEAGLLayer*)[view_handle layer];
    glBindRenderbuffer(GL_RENDERBUFFER, color_renderbuffer);

    if (![ctx_handle renderbufferStorage:GL_RENDERBUFFER fromDrawable:layer])
        Process_Abort("Failed to link renderbuffer to window");
}

static void UpdateDepthbuffer(void) {
    int backingW = 0, backingH = 0;

    // In case layer dimensions are different
    glBindRenderbuffer(GL_RENDERBUFFER, color_renderbuffer);
    glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_WIDTH,  &backingW);
    glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_HEIGHT, &backingH);

    // Shouldn't happen but just in case
    if (backingW <= 0) backingW = Window_Main.Width;
    if (backingH <= 0) backingH = Window_Main.Height;

    glBindRenderbuffer(GL_RENDERBUFFER, depth_renderbuffer);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24_OES, backingW, backingH);
}

static void CreateFramebuffer(void) {
    glGenFramebuffers(1, &framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);

    glGenRenderbuffers(1, &color_renderbuffer);
    UpdateColorbuffer();
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, color_renderbuffer);

    glGenRenderbuffers(1, &depth_renderbuffer);
    UpdateDepthbuffer();
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,  GL_RENDERBUFFER, depth_renderbuffer);

    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE)
        Process_Abort2(status, "Failed to create renderbuffer");

    fb_width  = Window_Main.Width;
    fb_height = Window_Main.Height;
}

void GLContext_Create(void) {
    ctx_handle = [[EAGLContext alloc] initWithAPI:kEAGLRenderingAPIOpenGLES2];
    [EAGLContext setCurrentContext:ctx_handle];

    // unlike other platforms, have to manually setup render framebuffer
    CreateFramebuffer();
	[ctx_handle autorelease];
}

void GLContext_Update(void) {
    // only resize buffers when absolutely have to
    if (fb_width == Window_Main.Width && fb_height == Window_Main.Height) return;
    fb_width  = Window_Main.Width;
    fb_height = Window_Main.Height;

    UpdateColorbuffer();
    UpdateDepthbuffer();
}

void GLContext_Free(void) {
    glDeleteRenderbuffers(1, &color_renderbuffer); color_renderbuffer = 0;
    glDeleteRenderbuffers(1, &depth_renderbuffer); depth_renderbuffer = 0;
    glDeleteFramebuffers(1, &framebuffer);         framebuffer        = 0;

    [EAGLContext setCurrentContext:Nil];
}

cc_bool GLContext_TryRestore(void) { return false; }
void* GLContext_GetAddress(const char* function) { return NULL; }

cc_bool GLContext_SwapBuffers(void) {
    static GLenum discards[] = { GL_DEPTH_ATTACHMENT };
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glDiscardFramebufferEXT(GL_FRAMEBUFFER, 1, discards);
    glBindRenderbuffer(GL_RENDERBUFFER, color_renderbuffer);

    [ctx_handle presentRenderbuffer:GL_RENDERBUFFER];
    return true;
}

void GLContext_SetVSync(cc_bool vsync) { }
void GLContext_GetApiInfo(cc_string* info) { }


@implementation CC3DView
+ (Class)layerClass {
    return [CAEAGLLayer class];
}
@end

static void Init3DLayer(void) {
    CAEAGLLayer* layer = (CAEAGLLayer*)[view_handle layer];

    [layer setOpaque:YES];
    // render at full physical pixel resolution for crisp Retina output
    [layer setContentsScale:display_scale];
    NSDictionary* props =
   @{
        kEAGLDrawablePropertyRetainedBacking : [NSNumber numberWithBool:NO],
        kEAGLDrawablePropertyColorFormat : kEAGLColorFormatRGBA8
    };
    [layer setDrawableProperties:props];
}
#endif


/*########################################################################################################################*
*-------------------------------------------------------Framebuffer-------------------------------------------------------*
*#########################################################################################################################*/
void Window_AllocFramebuffer(struct Bitmap* bmp, int width, int height) {
    bmp->width  = width;
    bmp->height = height;
    bmp->scan0  = (BitmapCol*)Mem_Alloc(width * height, BITMAPCOLOR_SIZE, "window pixels");

    win_ctx = CGBitmapContextCreate(bmp->scan0, width, height, 8, width * 4,
                                    CGColorSpaceCreateDeviceRGB(), kCGBitmapByteOrder32Host | kCGImageAlphaNoneSkipFirst);
}

void Window_DrawFramebuffer(Rect2D r, struct Bitmap* bmp) {
    CGImageRef image = CGBitmapContextCreateImage(win_ctx);
    CALayer* layer = [view_handle layer];
    [layer setContents:CFBridgingRelease(image)];
}

void Window_FreeFramebuffer(struct Bitmap* bmp) {
    Mem_Free(bmp->scan0);
    CGContextRelease(win_ctx);
}
