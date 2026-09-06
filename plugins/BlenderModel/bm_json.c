/* Minimal JSON parser for the BlenderModel plugin.
 * Licensed under BSD-3, same as ClassiCube.
 */
#include "src/PluginAPI.h"
#include "src/Platform.h"
#include "bm_json.h"

struct BMJ_Parser { char* text; int pos, len; struct BMJ_Doc* doc; cc_bool failed; };

static int BMJ_AddNode(struct BMJ_Parser* p, int type, int parent) {
	struct BMJ_Doc* doc = p->doc;
	struct BMJ_Node* node;
	if (doc->count >= doc->capacity) {
		int newCap = doc->capacity ? doc->capacity * 2 : 256;
		void* mem  = Mem_TryRealloc(doc->nodes, newCap, sizeof(struct BMJ_Node));
		if (!mem) { p->failed = true; return -1; }
		doc->nodes    = (struct BMJ_Node*)mem;
		doc->capacity = newCap;
	}

	node = &doc->nodes[doc->count];
	Mem_Set(node, 0, sizeof(*node));
	node->type   = type;
	node->parent = parent;
	node->next   = -1;
	node->firstChild = -1;
	return doc->count++;
}

/* Appends child to parent's child list (keeps insertion order) */
static void BMJ_Link(struct BMJ_Doc* doc, int parent, int child, int* lastChild) {
	if (*lastChild < 0) { doc->nodes[parent].firstChild = child; }
	else { doc->nodes[*lastChild].next = child; }
	*lastChild = child;
	doc->nodes[parent].childCount++;
}

static void BMJ_SkipSpace(struct BMJ_Parser* p) {
	while (p->pos < p->len) {
		char c = p->text[p->pos];
		if (c != ' ' && c != '\t' && c != '\r' && c != '\n') break;
		p->pos++;
	}
}

static int BMJ_Hex(char c) {
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

/* Parses a string literal, unescaping in place. p->pos must be at the opening quote */
static cc_bool BMJ_ParseString(struct BMJ_Parser* p, cc_string* out) {
	char* text = p->text;
	int start, write, i = p->pos + 1;

	start = i; write = i;
	while (i < p->len) {
		char c = text[i];
		if (c == '"') {
			out->buffer   = &text[start];
			out->length   = write - start;
			out->capacity = out->length;
			p->pos = i + 1;
			return true;
		}
		if (c != '\\') { text[write++] = c; i++; continue; }

		if (i + 1 >= p->len) return false;
		c = text[i + 1]; i += 2;
		switch (c) {
		case '"':  text[write++] = '"';  break;
		case '\\': text[write++] = '\\'; break;
		case '/':  text[write++] = '/';  break;
		case 'b':  text[write++] = '\b'; break;
		case 'f':  text[write++] = '\f'; break;
		case 'n':  text[write++] = '\n'; break;
		case 'r':  text[write++] = '\r'; break;
		case 't':  text[write++] = '\t'; break;
		case 'u': {
			int k, cp = 0, h;
			if (i + 4 > p->len) return false;
			for (k = 0; k < 4; k++) {
				h = BMJ_Hex(text[i + k]);
				if (h < 0) return false;
				cp = (cp << 4) | h;
			}
			i += 4;
			/* UTF-8 encode (surrogate pairs are left as two 3 byte sequences) */
			if (cp < 0x80) {
				text[write++] = (char)cp;
			} else if (cp < 0x800) {
				text[write++] = (char)(0xC0 | (cp >> 6));
				text[write++] = (char)(0x80 | (cp & 0x3F));
			} else {
				text[write++] = (char)(0xE0 | (cp >> 12));
				text[write++] = (char)(0x80 | ((cp >> 6) & 0x3F));
				text[write++] = (char)(0x80 | (cp & 0x3F));
			}
			break;
		}
		default: return false;
		}
	}
	return false;
}

static cc_bool BMJ_ParseNumber(struct BMJ_Parser* p, double* out) {
	char* text = p->text;
	int i = p->pos, digits = 0, expVal = 0;
	double sign = 1.0, value = 0.0, frac = 0.1;
	cc_bool expNeg = false;

	if (i < p->len && text[i] == '-') { sign = -1.0; i++; }
	while (i < p->len && text[i] >= '0' && text[i] <= '9') { value = value * 10.0 + (text[i] - '0'); i++; digits++; }
	if (i < p->len && text[i] == '.') {
		i++;
		while (i < p->len && text[i] >= '0' && text[i] <= '9') { value += (text[i] - '0') * frac; frac *= 0.1; i++; digits++; }
	}
	if (!digits) return false;

	if (i < p->len && (text[i] == 'e' || text[i] == 'E')) {
		i++;
		if (i < p->len && (text[i] == '+' || text[i] == '-')) { expNeg = text[i] == '-'; i++; }
		digits = 0;
		while (i < p->len && text[i] >= '0' && text[i] <= '9') { expVal = expVal * 10 + (text[i] - '0'); i++; digits++; }
		if (!digits) return false;
	}
	while (expVal-- > 0) value = expNeg ? value * 0.1 : value * 10.0;

	*out   = sign * value;
	p->pos = i;
	return true;
}

static cc_bool BMJ_Match(struct BMJ_Parser* p, const char* word) {
	int i;
	for (i = 0; word[i]; i++) {
		if (p->pos + i >= p->len || p->text[p->pos + i] != word[i]) return false;
	}
	p->pos += i;
	return true;
}

static int BMJ_ParseValue(struct BMJ_Parser* p, int parent, int depth);

static cc_bool BMJ_ParseMembers(struct BMJ_Parser* p, int node, int depth) {
	int lastChild = -1, child;
	cc_string key;

	p->pos++; /* { */
	BMJ_SkipSpace(p);
	if (p->pos < p->len && p->text[p->pos] == '}') { p->pos++; return true; }

	for (;;) {
		BMJ_SkipSpace(p);
		if (p->pos >= p->len || p->text[p->pos] != '"') return false;
		if (!BMJ_ParseString(p, &key)) return false;
		BMJ_SkipSpace(p);
		if (p->pos >= p->len || p->text[p->pos] != ':') return false;
		p->pos++;

		child = BMJ_ParseValue(p, node, depth + 1);
		if (child < 0) return false;
		p->doc->nodes[child].key = key;
		BMJ_Link(p->doc, node, child, &lastChild);

		BMJ_SkipSpace(p);
		if (p->pos >= p->len) return false;
		if (p->text[p->pos] == ',') { p->pos++; continue; }
		if (p->text[p->pos] == '}') { p->pos++; return true; }
		return false;
	}
}

static cc_bool BMJ_ParseElements(struct BMJ_Parser* p, int node, int depth) {
	int lastChild = -1, child;

	p->pos++; /* [ */
	BMJ_SkipSpace(p);
	if (p->pos < p->len && p->text[p->pos] == ']') { p->pos++; return true; }

	for (;;) {
		child = BMJ_ParseValue(p, node, depth + 1);
		if (child < 0) return false;
		BMJ_Link(p->doc, node, child, &lastChild);

		BMJ_SkipSpace(p);
		if (p->pos >= p->len) return false;
		if (p->text[p->pos] == ',') { p->pos++; continue; }
		if (p->text[p->pos] == ']') { p->pos++; return true; }
		return false;
	}
}

static int BMJ_ParseValue(struct BMJ_Parser* p, int parent, int depth) {
	int node;
	char c;
	if (depth > 64) return -1;
	BMJ_SkipSpace(p);
	if (p->pos >= p->len) return -1;
	c = p->text[p->pos];

	if (c == '{') {
		node = BMJ_AddNode(p, BMJ_OBJECT, parent);
		if (node < 0 || !BMJ_ParseMembers(p, node, depth)) return -1;
		return node;
	}
	if (c == '[') {
		node = BMJ_AddNode(p, BMJ_ARRAY, parent);
		if (node < 0 || !BMJ_ParseElements(p, node, depth)) return -1;
		return node;
	}
	if (c == '"') {
		cc_string str;
		if (!BMJ_ParseString(p, &str)) return -1;
		node = BMJ_AddNode(p, BMJ_STRING, parent);
		if (node < 0) return -1;
		p->doc->nodes[node].str = str;
		return node;
	}
	if (c == '-' || (c >= '0' && c <= '9')) {
		double num;
		if (!BMJ_ParseNumber(p, &num)) return -1;
		node = BMJ_AddNode(p, BMJ_NUMBER, parent);
		if (node < 0) return -1;
		p->doc->nodes[node].num = num;
		return node;
	}
	if (BMJ_Match(p, "true"))  { node = BMJ_AddNode(p, BMJ_BOOL, parent); if (node >= 0) p->doc->nodes[node].num = 1; return node; }
	if (BMJ_Match(p, "false")) { return BMJ_AddNode(p, BMJ_BOOL, parent); }
	if (BMJ_Match(p, "null"))  { return BMJ_AddNode(p, BMJ_NULL, parent); }
	return -1;
}

cc_bool BMJ_Parse(struct BMJ_Doc* doc, char* text, int len) {
	struct BMJ_Parser p;
	int root;
	Mem_Set(doc, 0, sizeof(*doc));
	p.text = text; p.pos = 0; p.len = len; p.doc = doc; p.failed = false;

	root = BMJ_ParseValue(&p, -1, 0);
	if (root != 0 || p.failed) { BMJ_Free(doc); return false; }
	BMJ_SkipSpace(&p);
	if (p.pos != p.len) { BMJ_Free(doc); return false; }
	return true;
}

void BMJ_Free(struct BMJ_Doc* doc) {
	Mem_Free(doc->nodes);
	doc->nodes = NULL; doc->count = 0; doc->capacity = 0;
}

int BMJ_Get(const struct BMJ_Doc* doc, int obj, const char* key) {
	int child;
	if (obj < 0 || obj >= doc->count || doc->nodes[obj].type != BMJ_OBJECT) return -1;

	for (child = doc->nodes[obj].firstChild; child >= 0; child = doc->nodes[child].next) {
		/* JSON keys are case sensitive, so compare exactly */
		const cc_string* k = &doc->nodes[child].key;
		int i;
		cc_bool same = true;
		for (i = 0; i < k->length && same; i++) same = key[i] != '\0' && k->buffer[i] == key[i];
		if (same && key[k->length] == '\0') return child;
	}
	return -1;
}

int BMJ_At(const struct BMJ_Doc* doc, int arr, int index) {
	int child, i = 0;
	if (arr < 0 || arr >= doc->count || doc->nodes[arr].type != BMJ_ARRAY || index < 0) return -1;

	for (child = doc->nodes[arr].firstChild; child >= 0; child = doc->nodes[child].next, i++) {
		if (i == index) return child;
	}
	return -1;
}

int BMJ_Count(const struct BMJ_Doc* doc, int node) {
	if (node < 0 || node >= doc->count) return 0;
	return doc->nodes[node].childCount;
}

int BMJ_GetAt(const struct BMJ_Doc* doc, int obj, const char* key, int index) {
	return BMJ_At(doc, BMJ_Get(doc, obj, key), index);
}

double BMJ_Num(const struct BMJ_Doc* doc, int node, double def) {
	if (node < 0 || node >= doc->count) return def;
	if (doc->nodes[node].type != BMJ_NUMBER && doc->nodes[node].type != BMJ_BOOL) return def;
	return doc->nodes[node].num;
}

int BMJ_Int(const struct BMJ_Doc* doc, int node, int def) {
	double v = BMJ_Num(doc, node, (double)def);
	return (int)(v < 0 ? v - 0.5 : v + 0.5);
}

double BMJ_GetNum(const struct BMJ_Doc* doc, int obj, const char* key, double def) {
	return BMJ_Num(doc, BMJ_Get(doc, obj, key), def);
}

int BMJ_GetInt(const struct BMJ_Doc* doc, int obj, const char* key, int def) {
	return BMJ_Int(doc, BMJ_Get(doc, obj, key), def);
}

cc_string BMJ_Str(const struct BMJ_Doc* doc, int node) {
	if (node < 0 || node >= doc->count || doc->nodes[node].type != BMJ_STRING) return String_Empty;
	return doc->nodes[node].str;
}

cc_string BMJ_GetStr(const struct BMJ_Doc* doc, int obj, const char* key) {
	return BMJ_Str(doc, BMJ_Get(doc, obj, key));
}

cc_bool BMJ_StrEquals(const struct BMJ_Doc* doc, int node, const char* value) {
	cc_string str = BMJ_Str(doc, node);
	int i;
	if (node < 0 || node >= doc->count || doc->nodes[node].type != BMJ_STRING) return false;
	for (i = 0; i < str.length; i++) {
		if (value[i] != str.buffer[i]) return false;
	}
	return value[str.length] == '\0';
}
