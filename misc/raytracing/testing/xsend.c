/* Minimal XTest input injector: xsend key <keysym> | keydown <keysym> | keyup <keysym> | motion <dx> <dy> | type <text> */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <unistd.h>
#include <X11/Xlib.h>
#include <X11/keysym.h>

typedef int (*FakeKey)(Display*, unsigned int, int, unsigned long);
typedef int (*FakeMotion)(Display*, int, int, int, unsigned long);
typedef int (*FakeButton)(Display*, unsigned int, int, unsigned long);

int main(int argc, char** argv) {
	void* xtst = dlopen("libXtst.so.6", RTLD_NOW);
	if (!xtst) { puts("no libXtst"); return 1; }
	FakeKey fakeKey = (FakeKey)dlsym(xtst, "XTestFakeKeyEvent");
	FakeMotion fakeMotion = (FakeMotion)dlsym(xtst, "XTestFakeRelativeMotionEvent");
	FakeButton fakeButton = (FakeButton)dlsym(xtst, "XTestFakeButtonEvent");
	Display* d = XOpenDisplay(NULL);
	if (!d) { puts("no display"); return 1; }

	if (argc < 2) return 1;
	if (!strcmp(argv[1], "key") || !strcmp(argv[1], "keydown") || !strcmp(argv[1], "keyup")) {
		KeySym ks = XStringToKeysym(argv[2]);
		KeyCode kc = XKeysymToKeycode(d, ks);
		if (!kc) { printf("unknown key %s\n", argv[2]); return 1; }
		if (strcmp(argv[1], "keyup"))   fakeKey(d, kc, True, 0);
		XFlush(d);
		if (!strcmp(argv[1], "key")) usleep(60000);
		if (strcmp(argv[1], "keydown")) fakeKey(d, kc, False, 0);
	} else if (!strcmp(argv[1], "motion")) {
		fakeMotion(d, 0, atoi(argv[2]), atoi(argv[3]), 0);
	} else if (!strcmp(argv[1], "click")) {
		fakeButton(d, atoi(argv[2]), True, 0); XFlush(d); usleep(50000);
		fakeButton(d, atoi(argv[2]), False, 0);
	} else if (!strcmp(argv[1], "type")) {
		const char* s = argv[2];
		for (; *s; s++) {
			char name[2] = { *s, 0 };
			KeySym ks = (*s == ' ') ? XK_space : (*s == '/') ? XK_slash : (*s == '-') ? XK_minus : (*s == '.') ? XK_period : XStringToKeysym(name);
			KeyCode kc = XKeysymToKeycode(d, ks);
			if (!kc) continue;
			fakeKey(d, kc, True, 0); XFlush(d); usleep(30000);
			fakeKey(d, kc, False, 0); XFlush(d); usleep(30000);
		}
	}
	XFlush(d);
	XSync(d, False);
	return 0;
}
