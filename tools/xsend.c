/* xsend WINDOW-ID ACTION...: synthetic input to one window (XSendEvent), for testing
 * without moving the real pointer. Actions: click X Y [BUTTON] | press X Y [BUTTON] | move X Y |
 * release X Y | type TEXT | key KEYSYM[+ctrl|+alt|+shift] | sleep MS */
#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static Display *d;
static Window w;
static int px, py;

static void button(int type, int x, int y, int b)
{
    XEvent e;
    memset(&e, 0, sizeof e);
    e.xbutton.type = type;
    e.xbutton.display = d;
    e.xbutton.window = w;
    e.xbutton.root = DefaultRootWindow(d);
    e.xbutton.x = x;
    e.xbutton.y = y;
    e.xbutton.button = b;
    {
        /* where on the screen: a context menu opens there */
        Window child;
        XTranslateCoordinates(d, w, DefaultRootWindow(d), x, y, &e.xbutton.x_root, &e.xbutton.y_root, &child);
    }
    e.xbutton.state = type == ButtonRelease ? (Button1Mask << (b - 1)) : 0;
    e.xbutton.same_screen = True;
    XSendEvent(d, w, True, type == ButtonPress ? ButtonPressMask : ButtonReleaseMask, &e);
    px = x;
    py = y;
}

static void motion(int x, int y, unsigned state)
{
    XEvent e;
    memset(&e, 0, sizeof e);
    e.xmotion.type = MotionNotify;
    e.xmotion.display = d;
    e.xmotion.window = w;
    e.xmotion.root = DefaultRootWindow(d);
    e.xmotion.x = x;
    e.xmotion.y = y;
    e.xmotion.state = state;
    e.xmotion.same_screen = True;
    XSendEvent(d, w, True, PointerMotionMask, &e);
    px = x;
    py = y;
}

static void key(KeySym sym, unsigned state)
{
    XEvent e;
    KeyCode kc = XKeysymToKeycode(d, sym);
    int type;
    if (!kc) {
        fprintf(stderr, "xsend: no keycode for keysym %lx\n", (unsigned long)sym);
        return;
    }
    if (sym >= 'A' && sym <= 'Z')
        state |= ShiftMask;
    for (type = KeyPress; type <= KeyRelease; type++) {
        memset(&e, 0, sizeof e);
        e.xkey.type = type;
        e.xkey.display = d;
        e.xkey.window = w;
        e.xkey.root = DefaultRootWindow(d);
        e.xkey.x = px;
        e.xkey.y = py;
        e.xkey.state = state;
        e.xkey.keycode = kc;
        e.xkey.same_screen = True;
        XSendEvent(d, w, True, type == KeyPress ? KeyPressMask : KeyReleaseMask, &e);
        XFlush(d);
    }
}

int main(int argc, char **argv)
{
    int i;
    if (argc < 3 || !(d = XOpenDisplay(NULL))) {
        fprintf(stderr, "usage: xsend WINDOW-ID ACTION...\n");
        return 2;
    }
    w = (Window)strtoul(argv[1], NULL, 0);
    for (i = 2; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "click") && i + 2 < argc) {
            int x = atoi(argv[i + 1]), y = atoi(argv[i + 2]), b = 1;
            i += 2;
            if (i + 1 < argc && strlen(argv[i + 1]) == 1 && argv[i + 1][0] >= '1' && argv[i + 1][0] <= '7')
                b = atoi(argv[++i]);
            button(ButtonPress, x, y, b);
            XFlush(d);
            usleep(50000);
            button(ButtonRelease, x, y, b);
        } else if ((!strcmp(a, "press") || !strcmp(a, "release")) && i + 2 < argc) {
            int b = 1;
            button(a[0] == 'p' ? ButtonPress : ButtonRelease, atoi(argv[i + 1]), atoi(argv[i + 2]),
                i + 3 < argc && strlen(argv[i + 3]) == 1 && argv[i + 3][0] >= '1' && argv[i + 3][0] <= '7' ? (b = atoi(argv[i + 3])) : 1);
            i += 2 + (b != 1);
        } else if (!strcmp(a, "move") && i + 2 < argc) {
            motion(atoi(argv[i + 1]), atoi(argv[i + 2]), Button1Mask);
            i += 2;
        } else if (!strcmp(a, "type") && i + 1 < argc) {
            const char *t = argv[++i];
            for (; *t; t++) {
                KeySym s = *t == ' ' ? XK_space : *t == '.' ? XK_period : *t == '/' ? XK_slash
                    : *t == ':' ? XK_colon : *t == '-' ? XK_minus : (KeySym)(unsigned char)*t;
                key(s, *t == ':' ? ShiftMask : 0);
            }
        } else if (!strcmp(a, "key") && i + 1 < argc) {
            char name[64], *plus;
            unsigned state = 0;
            strncpy(name, argv[++i], sizeof name - 1);
            name[sizeof name - 1] = 0;
            while ((plus = strrchr(name, '+'))) {
                if (!strcmp(plus, "+ctrl"))
                    state |= ControlMask;
                else if (!strcmp(plus, "+alt"))
                    state |= Mod1Mask;
                else if (!strcmp(plus, "+shift"))
                    state |= ShiftMask;
                *plus = 0;
            }
            key(XStringToKeysym(name), state);
        } else if (!strcmp(a, "sleep") && i + 1 < argc)
            usleep(atoi(argv[++i]) * 1000);
        XFlush(d);
    }
    XCloseDisplay(d);
    return 0;
}
