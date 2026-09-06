/* Minimal JSON parser for the BlenderModel plugin (used by the glTF loader).
 * Parses a document into a flat array of nodes; strings point into the
 * (unescaped in place) source text, which must stay alive while in use.
 * Licensed under BSD-3, same as ClassiCube.
 */
#ifndef BM_JSON_H
#define BM_JSON_H
#include "src/String_.h"

enum BMJ_Type { BMJ_NULL, BMJ_BOOL, BMJ_NUMBER, BMJ_STRING, BMJ_ARRAY, BMJ_OBJECT };

struct BMJ_Node {
	cc_uint8 type;
	int parent, next, firstChild, childCount;
	cc_string key; /* only set for members of an object */
	cc_string str; /* BMJ_STRING */
	double num;    /* BMJ_NUMBER and BMJ_BOOL */
};

struct BMJ_Doc {
	struct BMJ_Node* nodes;
	int count, capacity;
};

/* Parses text (modified in place) into doc; node 0 is the root value */
cc_bool BMJ_Parse(struct BMJ_Doc* doc, char* text, int len);
void    BMJ_Free(struct BMJ_Doc* doc);

/* All lookups return a node index, or -1 if missing / wrong type */
int BMJ_Get(const struct BMJ_Doc* doc, int obj, const char* key);
int BMJ_At(const struct BMJ_Doc* doc, int arr, int index);
int BMJ_Count(const struct BMJ_Doc* doc, int node);
/* Convenience for objects nested under a key: BMJ_At(BMJ_Get(obj, key), index) */
int BMJ_GetAt(const struct BMJ_Doc* doc, int obj, const char* key, int index);

double    BMJ_Num(const struct BMJ_Doc* doc, int node, double def);
int       BMJ_Int(const struct BMJ_Doc* doc, int node, int def);
/* Value of key in obj as a number/int, or def */
double    BMJ_GetNum(const struct BMJ_Doc* doc, int obj, const char* key, double def);
int       BMJ_GetInt(const struct BMJ_Doc* doc, int obj, const char* key, int def);
cc_string BMJ_Str(const struct BMJ_Doc* doc, int node);
cc_string BMJ_GetStr(const struct BMJ_Doc* doc, int obj, const char* key);
cc_bool   BMJ_StrEquals(const struct BMJ_Doc* doc, int node, const char* value);
#endif
