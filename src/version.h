/*
 * GBWUI — Application version
 *
 * Bump MAJOR/MINOR/PATCH manually before tagging a release.
 * The build system injects APP_VERSION_STRING via -D when git-describe
 * is available (e.g. "v1.0.0-3-gabcdef"); the macro below is the fallback.
 */
#ifndef APP_VERSION_H
#define APP_VERSION_H

#define APP_VERSION_MAJOR 1
#define APP_VERSION_MINOR 1
#define APP_VERSION_PATCH 1

/* Stringify helpers */
#define _VER_STR(x) #x
#define _VER_XSTR(x) _VER_STR(x)

#ifndef APP_VERSION_STRING
#define APP_VERSION_STRING \
    "v" _VER_XSTR(APP_VERSION_MAJOR) "." _VER_XSTR(APP_VERSION_MINOR) "." _VER_XSTR(APP_VERSION_PATCH)
#endif

#endif /* APP_VERSION_H */
