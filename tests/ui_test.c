/*
 * Headless LV2 UI test host (X11 + XTest, run under Xvfb).
 *
 * Embeds an X11UI in a parent window, then runs a script of commands.
 * Coordinates are in the pedal's design space and mapped to the UI's
 * current window size (the UI scales the art uniformly and centres it).
 *
 *   ui_test <ui.so> <plugin-uri> <bundle-dir> <designW>x<designH>
 *           [portmap:SYM=IDX,...] cmd...
 *
 *   portmap: symbols the ui:portMap feature resolves (DPF uses it to find
 *            lv2_enabled and invert it into its bypass parameter)
 *
 *   set:PORT=VALUE       host -> UI parameter value (port_event)
 *   shot:FILE.png        screenshot of the UI window
 *   resize:WxH           resize the UI window
 *   drag:X,Y,DY          press at (X,Y), move DY design units, release
 *   click:X,Y            click at (X,Y)
 *   dclick:X,Y           double-click at (X,Y)
 *   wheel:X,Y,N          N wheel steps up (N<0: down)
 *   expect:PORT<op>V     check the last value the UI wrote to PORT,
 *                        op is one of < > = (=: within 1e-3)
 *   size:WxH             check the UI window size
 *
 * Copyright (C) 2026 Dataforte
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <X11/Xlib.h>
#include <X11/extensions/XTest.h>
#include <dlfcn.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "lv2host.h"

typedef void *LV2UI_Handle;
typedef void *LV2UI_Controller;
typedef void *LV2UI_Widget;
typedef void (*LV2UI_Write_Function)(LV2UI_Controller, uint32_t, uint32_t, uint32_t, const void *);

typedef struct LV2UI_Descriptor {
  const char *URI;
  LV2UI_Handle (*instantiate)(const struct LV2UI_Descriptor *, const char *, const char *,
                              LV2UI_Write_Function, LV2UI_Controller, LV2UI_Widget *,
                              const LV2_Feature *const *);
  void (*cleanup)(LV2UI_Handle);
  void (*port_event)(LV2UI_Handle, uint32_t, uint32_t, uint32_t, const void *);
  const void *(*extension_data)(const char *);
} LV2UI_Descriptor;

typedef struct { int (*idle)(LV2UI_Handle); } LV2UI_Idle_Interface;
typedef struct { void *handle; int (*ui_resize)(void *, int, int); } LV2UI_Resize;
typedef struct { void *handle; uint32_t (*port_index)(void *, const char *); } LV2UI_Port_Map;

#define MAX_SYMS 16
static char map_sym[MAX_SYMS][32];
static uint32_t map_idx[MAX_SYMS];
static int n_syms = 0;

static uint32_t
host_port_index(void *h, const char *sym)
{
  (void)h;
  for (int i = 0; i < n_syms; ++i)
    if (strcmp(map_sym[i], sym) == 0)
      return map_idx[i];
  return (uint32_t)-1;   /* LV2UI_INVALID_PORT_INDEX */
}

static Display *dpy;
static Window parent, child;
static LV2UI_Handle ui;
static const LV2UI_Descriptor *desc;
static const LV2UI_Idle_Interface *idle_iface;
static double design_w, design_h;

#define MAX_PORTS 64
static float written[MAX_PORTS];
static int was_written[MAX_PORTS];
static int failures = 0;

static void
write_fn(LV2UI_Controller c, uint32_t port, uint32_t size, uint32_t proto, const void *buf)
{
  (void)c;
  if (proto == 0 && size == sizeof(float) && port < MAX_PORTS) {
    written[port] = *(const float *)buf;
    was_written[port] = 1;
  }
}

static int
host_resize(void *h, int w, int hgt)
{
  (void)h;
  XResizeWindow(dpy, parent, (unsigned)w, (unsigned)hgt);
  return 0;
}

static void
pump(int ms)
{
  for (int t = 0; t < ms; t += 10) {
    if (idle_iface)
      idle_iface->idle(ui);
    XSync(dpy, False);
    usleep(10000);
  }
}

static void
child_geometry(int *w, int *h)
{
  Window root;
  int x, y;
  unsigned cw, ch, bw, depth;
  XGetGeometry(dpy, child, &root, &x, &y, &cw, &ch, &bw, &depth);
  *w = (int)cw;
  *h = (int)ch;
}

/* design coordinates -> root (screen) coordinates */
static void
to_screen(double dx, double dy, int *sx, int *sy)
{
  int w, h;
  child_geometry(&w, &h);
  const double s = fmin(w / design_w, h / design_h);
  const double ox = (w - design_w * s) / 2, oy = (h - design_h * s) / 2;
  Window dummy;
  XTranslateCoordinates(dpy, child, DefaultRootWindow(dpy),
                        (int)lround(ox + dx * s), (int)lround(oy + dy * s), sx, sy, &dummy);
}

static void
check(int ok, const char *msg)
{
  printf("%s: %s\n", ok ? "PASS" : "FAIL", msg);
  if (!ok)
    ++failures;
}

int
main(int argc, char **argv)
{
  if (argc < 5) {
    fprintf(stderr, "usage: %s ui.so plugin-uri bundle-dir WxH cmd...\n", argv[0]);
    return 2;
  }
  if (sscanf(argv[4], "%lfx%lf", &design_w, &design_h) != 2)
    return 2;

  dpy = XOpenDisplay(NULL);
  if (!dpy) {
    fprintf(stderr, "cannot open X display\n");
    return 2;
  }
  parent = XCreateSimpleWindow(dpy, DefaultRootWindow(dpy), 0, 0, 1200, 1200, 0, 0, 0);
  XMapWindow(dpy, parent);
  XSync(dpy, False);

  void *lib = dlopen(argv[1], RTLD_NOW);
  if (!lib) {
    fprintf(stderr, "dlopen: %s\n", dlerror());
    return 2;
  }
  const LV2UI_Descriptor *(*fn)(uint32_t) =
    (const LV2UI_Descriptor *(*)(uint32_t))dlsym(lib, "lv2ui_descriptor");
  desc = fn ? fn(0) : NULL;
  check(desc != NULL, "lv2ui_descriptor(0)");
  if (!desc)
    return 1;

  int first_cmd = 5;
  if (argc > 5 && !strncmp(argv[5], "portmap:", 8)) {
    char buf[512];
    snprintf(buf, sizeof buf, "%s", argv[5] + 8);
    for (char *tok = strtok(buf, ","); tok && n_syms < MAX_SYMS; tok = strtok(NULL, ",")) {
      char *eq = strchr(tok, '=');
      if (!eq)
        continue;
      *eq = 0;
      snprintf(map_sym[n_syms], sizeof map_sym[0], "%s", tok);
      map_idx[n_syms++] = (uint32_t)atoi(eq + 1);
    }
    first_cmd = 6;
  }

  static LV2UI_Port_Map portmap = { NULL, host_port_index };
  static LV2_Feature f_portmap = { "http://lv2plug.in/ns/extensions/ui#portMap", &portmap };
  static LV2UI_Resize resize = { NULL, host_resize };
  static LV2_Feature f_parent = { "http://lv2plug.in/ns/extensions/ui#parent", NULL };
  static LV2_Feature f_resize = { "http://lv2plug.in/ns/extensions/ui#resize", &resize };
  f_parent.data = (void *)(uintptr_t)parent;
  const LV2_Feature *const *base = host_get_features(1024);
  const LV2_Feature *features[] = { base[0], base[1], &f_parent, &f_resize, &f_portmap, NULL };

  LV2UI_Widget widget = NULL;
  ui = desc->instantiate(desc, argv[2], argv[3], write_fn, NULL, &widget, features);
  check(ui != NULL && widget != NULL, "instantiate UI embedded in X11 parent");
  if (!ui)
    return 1;
  child = (Window)(uintptr_t)widget;
  idle_iface = desc->extension_data ?
    desc->extension_data("http://lv2plug.in/ns/extensions/ui#idleInterface") : NULL;
  check(idle_iface != NULL, "idle interface");
  XMapWindow(dpy, child);
  pump(300);

  int w, h;
  child_geometry(&w, &h);
  printf("      initial UI size %dx%d\n", w, h);

  for (int a = first_cmd; a < argc; ++a) {
    const char *c = argv[a];
    char msg[256];
    if (!strncmp(c, "set:", 4)) {
      unsigned port;
      float v;
      if (sscanf(c + 4, "%u=%f", &port, &v) == 2)
        desc->port_event(ui, port, sizeof(float), 0, &v);
      pump(150);
    } else if (!strncmp(c, "shot:", 5)) {
      pump(150);
      char cmd[512];
      snprintf(cmd, sizeof cmd, "import -window 0x%lx '%s'", (unsigned long)child, c + 5);
      check(system(cmd) == 0, c);
    } else if (!strncmp(c, "resize:", 7)) {
      unsigned rw, rh;
      if (sscanf(c + 7, "%ux%u", &rw, &rh) == 2) {
        XResizeWindow(dpy, parent, rw, rh);
        XResizeWindow(dpy, child, rw, rh);
      }
      pump(400);
    } else if (!strncmp(c, "size:", 5)) {
      int ew, eh;
      child_geometry(&w, &h);
      sscanf(c + 5, "%dx%d", &ew, &eh);
      snprintf(msg, sizeof msg, "UI size %dx%d (expected %dx%d)", w, h, ew, eh);
      check(w == ew && h == eh, msg);
    } else if (!strncmp(c, "drag:", 5)) {
      double x, y, dy;
      int sx, sy, ex, ey;
      if (sscanf(c + 5, "%lf,%lf,%lf", &x, &y, &dy) == 3) {
        to_screen(x, y, &sx, &sy);
        to_screen(x, y + dy, &ex, &ey);
        XTestFakeMotionEvent(dpy, -1, sx, sy, 0);
        pump(50);
        XTestFakeButtonEvent(dpy, 1, True, 0);
        pump(50);
        for (int i = 1; i <= 10; ++i) {
          XTestFakeMotionEvent(dpy, -1, sx, sy + (ey - sy) * i / 10, 0);
          pump(20);
        }
        XTestFakeButtonEvent(dpy, 1, False, 0);
        pump(400);   /* > double-click window */
      }
    } else if (!strncmp(c, "hover:", 6)) {
      double x, y;
      int sx, sy;
      if (sscanf(c + 6, "%lf,%lf", &x, &y) == 2) {
        to_screen(x, y, &sx, &sy);
        XTestFakeMotionEvent(dpy, -1, sx, sy, 0);
        pump(900);   /* long enough for the tooltip dwell time */
      }
    } else if (!strncmp(c, "click:", 6)) {
      double x, y;
      int sx, sy;
      if (sscanf(c + 6, "%lf,%lf", &x, &y) == 2) {
        to_screen(x, y, &sx, &sy);
        XTestFakeMotionEvent(dpy, -1, sx, sy, 0);
        pump(50);
        XTestFakeButtonEvent(dpy, 1, True, 0);
        pump(30);
        XTestFakeButtonEvent(dpy, 1, False, 0);
        pump(400);
      }
    } else if (!strncmp(c, "dclick:", 7)) {
      double x, y;
      int sx, sy;
      if (sscanf(c + 7, "%lf,%lf", &x, &y) == 2) {
        to_screen(x, y, &sx, &sy);
        XTestFakeMotionEvent(dpy, -1, sx, sy, 0);
        pump(50);
        for (int i = 0; i < 2; ++i) {
          XTestFakeButtonEvent(dpy, 1, True, 0);
          pump(20);
          XTestFakeButtonEvent(dpy, 1, False, 0);
          pump(40);
        }
        pump(400);
      }
    } else if (!strncmp(c, "wheel:", 6)) {
      double x, y;
      int n, sx, sy;
      if (sscanf(c + 6, "%lf,%lf,%d", &x, &y, &n) == 3) {
        to_screen(x, y, &sx, &sy);
        XTestFakeMotionEvent(dpy, -1, sx, sy, 0);
        pump(50);
        const unsigned btn = n > 0 ? 4 : 5;
        for (int i = 0; i < abs(n); ++i) {
          XTestFakeButtonEvent(dpy, btn, True, 0);
          XTestFakeButtonEvent(dpy, btn, False, 0);
          pump(30);
        }
        pump(200);
      }
    } else if (!strncmp(c, "expect:", 7)) {
      unsigned port;
      char op;
      float v;
      if (sscanf(c + 7, "%u%c%f", &port, &op, &v) == 3 && port < MAX_PORTS) {
        const float got = written[port];
        int ok = was_written[port] &&
                 ((op == '<' && got < v) || (op == '>' && got > v) ||
                  (op == '=' && fabsf(got - v) < 1e-3f));
        snprintf(msg, sizeof msg, "%s (UI wrote %s%.3f)", c,
                 was_written[port] ? "" : "nothing, last ", got);
        check(ok, msg);
      }
    } else {
      fprintf(stderr, "unknown command %s\n", c);
      return 2;
    }
  }

  desc->cleanup(ui);
  XDestroyWindow(dpy, parent);
  XCloseDisplay(dpy);
  printf("\n%s (%d failure%s)\n", failures ? "UI TESTS FAILED" : "ALL UI TESTS PASSED",
         failures, failures == 1 ? "" : "s");
  return failures ? 1 : 0;
}
