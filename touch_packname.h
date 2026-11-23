#ifndef TOUCH_PACKNAME_H
#define TOUCH_PACKNAME_H
#include <stddef.h>
#include <string.h>
/* A pack the server asks for without naming the map it is for (no "--for maps/<map>.bsp"): the one whose file name
 * carries the map's name (cts_mccormic-mark.pk3 for mccormic-mark; the name chain of tzs_find too), case aside. */
static inline int tpn_url_names_map(const char *url, const char *map)
{
	const char *b = strrchr(url, '/'), *q;
	char name[256], base[64];
	size_t i, len = strlen(map), n;
	if (!b || len < 4 || len >= sizeof(base)) return 0;
	b++;
	q = strpbrk(b, "?#");
	n = q ? (size_t)(q - b) : strlen(b);
	if (n >= sizeof(name)) return 0;
	for (i = 0; i < n; i++) name[i] = (char)((b[i] >= 'A' && b[i] <= 'Z') ? b[i] + 32 : b[i]);
	name[n] = 0;
	for (i = 0; i < len; i++) base[i] = (char)((map[i] >= 'A' && map[i] <= 'Z') ? map[i] + 32 : map[i]);
	base[len] = 0;
	if (strstr(name, base)) return 1;
	for (i = len; i-- > 4; )
		if (base[i] == '_') { base[i] = 0; if (strstr(name, base)) return 1; }
	return 0;
}
#endif
