/* SPDX-License-Identifier: MIT
 * JavaScript values on the CiukiOS code generation. QEMU 2026-10-25: the
 * first remote page with a script (example.com) ended NetSurf with
 * "vector=14 error=00000004 ... address=00000200" in
 * duk_hobject_find_entry_tval_ptr_stridx, a property lookup on an object at
 * a small integer address. test_g26.py builds this program with -m32 and the
 * SDK's code-generation flags (i686, x87 floating point, no SSE) against
 * NetSurf's own Duktape and configuration (duk_config.h + duk_custom.h) and
 * the port's JavaScript allocator.
 *
 * With Duktape's packed 8-byte duk_tval, a non-number value is a NaN whose
 * top 16 bits are the tag. The tags of fastint (0xfff1), undefined, null,
 * boolean, pointer and lightfunc (up to 0xfff7) have the quiet bit clear, so
 * they are signalling NaNs. Clang copies the union as a double; with x87 the
 * copy is fldl/fstpl, and fldl quiets a signalling NaN (Intel SDM, FLD): the
 * tag gains 0x0008. A fastint becomes 0xfff9, an object whose pointer is the
 * integer, so its first use reads or writes a few bytes above that address
 * (on this build a moved 476 faults at 0x1e0, its reference count); the
 * other kinds become numbers. Duktape documents the effect in
 * misc/clang_aliasing.c ("0xf1 into 0xf9"). duk_insert is one such copy and
 * runs in Duktape's calls (the undefined this binding) and in NetSurf's
 * bindings. Patch 0014 selects the unpacked duk_tval on 32-bit x86. */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "duktape.h"
#include "sdk/netsurf/port/js_memory.h"

/* NetSurf's duk_custom.h names this execution-timeout hook. */
duk_bool_t dukky_check_timeout(void *udata);
duk_bool_t dukky_check_timeout(void *udata) { (void)udata; return 0; }

/* As dukky.c: every allocation is charged to the shared JavaScript budget. */
static void *js_alloc(void *udata, duk_size_t size) { (void)udata; return size ? ciuki_js_alloc(size) : NULL; }
static void *js_realloc(void *udata, void *ptr, duk_size_t size)
{
	(void)udata;
	if (!size) { ciuki_js_free(ptr); return NULL; }
	return ciuki_js_realloc(ptr, size);
}
static void js_free(void *udata, void *ptr) { (void)udata; ciuki_js_free(ptr); }

enum { UNDEF, NUL, TRUE_, FALSE_, ZERO, INT476, MINUS1, INT_MIN48, DOUBLE, NOTANUMBER, POINTER, LIGHTFUNC, STRING, OBJECT, BUFFER, KINDS };
static const char *const names[KINDS] = { "undefined", "null", "true", "false", "fastint 0", "fastint 476",
	"fastint -1", "fastint -2^47", "double 1.5", "NaN", "pointer", "lightfunc", "string", "object", "buffer" };
static duk_ret_t lightfunc(duk_context *ctx) { (void)ctx; return 0; }

static void push_kind(duk_context *ctx, int kind)
{
	switch (kind) {
	case UNDEF: duk_push_undefined(ctx); break;
	case NUL: duk_push_null(ctx); break;
	case TRUE_: duk_push_true(ctx); break;
	case FALSE_: duk_push_false(ctx); break;
	case ZERO: duk_push_int(ctx, 0); break;
	case INT476: duk_push_int(ctx, 476); break;
	case MINUS1: duk_push_int(ctx, -1); break;
	case INT_MIN48: duk_push_number(ctx, -140737488355328.0); break;
	case DOUBLE: duk_push_number(ctx, 1.5); break;
	case NOTANUMBER: duk_push_nan(ctx); break;
	case POINTER: duk_push_pointer(ctx, (void *)(uintptr_t)0x1234); break;
	case LIGHTFUNC: duk_push_c_lightfunc(ctx, lightfunc, 0, 0, 0); break;
	case STRING: duk_push_string(ctx, "Ciuki"); break;
	case OBJECT: duk_push_object(ctx); duk_push_int(ctx, 7); duk_put_prop_string(ctx, -2, "dog"); break;
	case BUFFER: duk_push_fixed_buffer(ctx, 4); break;
	}
}

/* 1 when the value at idx is still the pushed kind; reads no property of a
 * value that is not an object (the QEMU fault). */
static int is_kind(duk_context *ctx, duk_idx_t idx, int kind)
{
	switch (kind) {
	case UNDEF: return duk_is_undefined(ctx, idx);
	case NUL: return duk_is_null(ctx, idx);
	case TRUE_: return duk_is_boolean(ctx, idx) && duk_get_boolean(ctx, idx);
	case FALSE_: return duk_is_boolean(ctx, idx) && !duk_get_boolean(ctx, idx);
	case ZERO: return duk_is_number(ctx, idx) && duk_get_number(ctx, idx) == 0.0;
	case INT476: return duk_is_number(ctx, idx) && duk_get_number(ctx, idx) == 476.0;
	case MINUS1: return duk_is_number(ctx, idx) && duk_get_number(ctx, idx) == -1.0;
	case INT_MIN48: return duk_is_number(ctx, idx) && duk_get_number(ctx, idx) == -140737488355328.0;
	case DOUBLE: return duk_is_number(ctx, idx) && duk_get_number(ctx, idx) == 1.5;
	case NOTANUMBER: return duk_is_number(ctx, idx) && isnan(duk_get_number(ctx, idx));
	case POINTER: return duk_is_pointer(ctx, idx) && duk_get_pointer(ctx, idx) == (void *)(uintptr_t)0x1234;
	case LIGHTFUNC: return duk_is_lightfunc(ctx, idx);
	case STRING: return duk_is_string(ctx, idx) && !strcmp(duk_get_string(ctx, idx), "Ciuki");
	case OBJECT:
		if (duk_get_type(ctx, idx) != DUK_TYPE_OBJECT) return 0;
		duk_get_prop_string(ctx, idx, "dog");
		{ int ok = duk_get_int(ctx, -1) == 7; duk_pop(ctx); return ok; }
	case BUFFER: return duk_is_buffer(ctx, idx);
	}
	return 0;
}

static unsigned failures;
static void expect(int ok, const char *operation, int kind, duk_context *ctx, duk_idx_t idx)
{
	if (ok) return;
	failures++;
	printf("FAIL %s: %s became type %d\n", operation, names[kind], (int)duk_get_type(ctx, idx));
}

/* A C function called from C or JavaScript: its this binding and its
 * argument arrive through Duktape's call setup. */
static int this_undefined;
static duk_ret_t echo(duk_context *ctx)
{
	duk_push_this(ctx);
	this_undefined = duk_is_undefined(ctx, -1);
	duk_pop(ctx);
	duk_dup(ctx, 0);
	return 1;
}
/* A binding that moves its integer result below a temporary, as dukky.c and
 * the generated bindings do with duk_insert. */
static duk_ret_t moved_integer(duk_context *ctx)
{
	duk_push_string(ctx, "temporary");
	duk_push_int(ctx, 476);
	duk_insert(ctx, -2);
	duk_pop(ctx);
	return 1;
}

int main(void)
{
	setvbuf(stdout, NULL, _IOLBF, 0);
	duk_context *ctx = duk_create_heap(js_alloc, js_realloc, js_free, NULL, NULL);
	if (!ctx) { puts("FAIL heap"); return 1; }
	printf("duk_tval: %s\n",
#if defined(DUK_USE_PACKED_TVAL)
	       "packed (8 bytes)"
#else
	       "unpacked"
#endif
	);
	for (int kind = 0; kind < KINDS; kind++) {
		/* duk_insert: the value moves below another one. */
		duk_set_top(ctx, 0);
		duk_push_string(ctx, "below");
		push_kind(ctx, kind);
		duk_insert(ctx, 0);
		expect(is_kind(ctx, 0, kind), "duk_insert", kind, ctx, 0);
		/* duk_swap, duk_replace, duk_copy, duk_dup. */
		duk_set_top(ctx, 0);
		push_kind(ctx, kind);
		duk_push_string(ctx, "other");
		duk_swap(ctx, 0, 1);
		expect(is_kind(ctx, 1, kind), "duk_swap", kind, ctx, 1);
		duk_dup(ctx, 1);
		duk_replace(ctx, 0);
		expect(is_kind(ctx, 0, kind), "duk_replace", kind, ctx, 0);
		duk_copy(ctx, 0, 1);
		expect(is_kind(ctx, 1, kind), "duk_copy", kind, ctx, 1);
		/* Array and object properties. */
		duk_set_top(ctx, 0);
		duk_push_array(ctx);
		push_kind(ctx, kind);
		duk_put_prop_index(ctx, 0, 3);
		duk_get_prop_index(ctx, 0, 3);
		expect(is_kind(ctx, -1, kind), "array element", kind, ctx, -1);
		duk_push_object(ctx);
		push_kind(ctx, kind);
		duk_put_prop_string(ctx, -2, "value");
		duk_get_prop_string(ctx, -1, "value");
		expect(is_kind(ctx, -1, kind), "object property", kind, ctx, -1);
		/* A C call: argument, return value and undefined this binding. */
		duk_set_top(ctx, 0);
		duk_push_c_function(ctx, echo, 1);
		push_kind(ctx, kind);
		this_undefined = 0;
		duk_call(ctx, 1);
		expect(is_kind(ctx, -1, kind), "duk_call argument/result", kind, ctx, -1);
		if (!this_undefined) { failures++; printf("FAIL duk_call: this binding is not undefined\n"); }
	}

	/* The QEMU fault's shape: a binding's integer that went through
	 * duk_insert is used by a script; with a quieted tag it is an object at
	 * address 476 (the script only asks its type, so a failure is reported
	 * instead of faulting). */
	duk_set_top(ctx, 0);
	duk_push_c_function(ctx, moved_integer, 0);
	duk_put_global_string(ctx, "movedInteger");
	if (duk_peval_string(ctx, "var v = movedInteger(); typeof v + ':' + (typeof v === 'number' ? v.toFixed(0) : '-')") != 0 ||
	    strcmp(duk_safe_to_string(ctx, -1), "number:476")) {
		failures++;
		printf("FAIL script read of a moved integer: %s\n", duk_safe_to_string(ctx, -1));
	}
	/* Integers, null and undefined through script code, arrays, closures
	 * and a C function called with call(). */
	duk_set_top(ctx, 0);
	duk_push_c_function(ctx, echo, 1);
	duk_put_global_string(ctx, "echo");
	if (duk_peval_string(ctx,
		"var langs = [], n = 0;"
		"for (var i = 0; i < 300; i++) langs.push({ lang: ['en', 'fr', 'zh'][i % 3], n: i, none: i % 2 ? null : undefined });"
		"var found = null;"
		"['de-DE', 'fr-FR'].some(function (l) { found = langs.filter(function (p) { return p.lang === l.split('-')[0]; })[0]; return found; });"
		"langs.forEach(function (p) { if (p.none === null) n += p.n; });"
		"found.n + ':' + n + ':' + echo.call(undefined, 476)") != 0 ||
	    strcmp(duk_safe_to_string(ctx, -1), "1:22500:476")) {
		failures++;
		printf("FAIL script values: %s\n", duk_safe_to_string(ctx, -1));
	}
	/* Duktape 2.7 parses ECMAScript 5.1 plus parts of ES2015: a script with
	 * spread or arrow functions (as example.com's s.js since 2026-10-24) is
	 * a SyntaxError, reported with its stack text as dukky_dump_error does. */
	duk_set_top(ctx, 0);
	static const char modern[] = "var P=[...document.body.querySelectorAll('p')];P.some(l=>l);";
	duk_push_string(ctx, "http://fixture.example/modern.js");
	if (duk_pcompile_lstring_filename(ctx, DUK_COMPILE_EVAL, modern, sizeof(modern) - 1) == 0 ||
	    strncmp(duk_safe_to_stacktrace(ctx, -1), "SyntaxError: parse error", 24)) {
		failures++;
		printf("FAIL ES2015 syntax: %s\n", duk_safe_to_string(ctx, -1));
	}
	duk_set_top(ctx, 0);
	duk_gc(ctx, 0);
	size_t charged = ciuki_js_charged();
	duk_destroy_heap(ctx);
	if (ciuki_js_charged() || ciuki_js_refusals()) { failures++; printf("FAIL allocator: charged=%zu refusals=%lu\n", ciuki_js_charged(), ciuki_js_refusals()); }
	if (failures) { printf("FAIL %u Duktape value checks\n", failures); return 1; }
	printf("PASS Duktape values on i686/x87: %d kinds through insert/swap/replace/copy/properties/calls, script integers, ES2015 SyntaxError; heap %zu bytes charged\n", KINDS, charged);
	return 0;
}
