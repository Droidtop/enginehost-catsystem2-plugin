/*
 * Shift-JIS (CP932) to UTF-8. Every string in a CatSystem2 game is CP932: the
 * scripts, the archive entry names, startup.xml. The engine works in UTF-8
 * internally so that everything above this line can ignore the question.
 */
#ifndef CS2_SJIS_H
#define CS2_SJIS_H

#include <stddef.h>
#include <stdint.h>

/*
 * Converts size bytes of CP932 into a freshly allocated UTF-8 string, always
 * terminated. A byte that is not valid CP932 becomes U+FFFD rather than
 * stopping the conversion, because one bad byte in a script must not lose the
 * line. Returns NULL only when out of memory.
 */
char *cs2_sjis_to_utf8(const uint8_t *data, size_t size);

#endif
