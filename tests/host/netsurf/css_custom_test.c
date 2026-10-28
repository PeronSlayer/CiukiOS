/* SPDX-License-Identifier: MIT
 * Appended to the pinned libcss selection driver by test_modern.py.
 * The independent oracle is an equivalent stylesheet without variables. */
#include "select/custom.h"
#include "allocation_fail.h"
static css_fixed inspected_row, inspected_column;
static css_unit inspected_row_unit, inspected_column_unit;
static uint8_t inspected_column_type;

static css_error custom_resolve(void *pw, const char *base, lwc_string *relative, lwc_string **absolute)
{
	(void)pw;
	if (memchr(lwc_string_data(relative), ':', lwc_string_length(relative)) != NULL) {
		*absolute = lwc_string_ref(relative);
		return CSS_OK;
	}
	char buffer[1024];
	const char *slash = strrchr(base, '/');
	int count = snprintf(buffer, sizeof(buffer), "%.*s%.*s", (int)(slash - base + 1), base,
			(int)lwc_string_length(relative), lwc_string_data(relative));
	assert(count > 0 && (size_t)count < sizeof(buffer));
	return lwc_intern_string(buffer, count, absolute) == lwc_error_ok ? CSS_OK : CSS_NOMEM;
}

static css_stylesheet *custom_sheet_at(const char *text, bool inlined, size_t chunk, const char *url)
{
	css_stylesheet_params params = {
		.params_version = CSS_STYLESHEET_PARAMS_VERSION_1,
		.level = CSS_LEVEL_21, .charset = "UTF-8", .url = url,
		.inline_style = inlined, .resolve = custom_resolve
	};
	css_stylesheet *sheet = NULL;
	assert(css_stylesheet_create(&params, &sheet) == CSS_OK);
	size_t length = strlen(text);
	for (size_t i = 0; i < length; i += chunk) {
		size_t count = length - i < chunk ? length - i : chunk;
		css_error error = css_stylesheet_append_data(sheet, (const uint8_t *)text + i, count);
		assert(error == CSS_OK || error == CSS_NEEDDATA);
	}
	css_error error = css_stylesheet_data_done(sheet);
	assert(error == CSS_OK || error == CSS_IMPORTS_PENDING);
	return sheet;
}

static css_stylesheet *custom_sheet(const char *text, bool inlined, size_t chunk)
{
	return custom_sheet_at(text, inlined, chunk, "file:///uses/use.css");
}

static node *custom_node(const char *name, node *parent)
{
	node *n = calloc(1, sizeof(*n));
	assert(n != NULL);
	assert(lwc_intern_string(name, strlen(name), &n->name) == lwc_error_ok);
	n->parent = parent;
	if (parent != NULL) {
		if (parent->last_child != NULL) {
			n->prev = parent->last_child;
			n->prev->next = n;
		} else parent->children = n;
		parent->last_child = n;
	}
	return n;
}

static void custom_select(css_select_ctx *select, node *n, line_ctx *ctx,
		css_stylesheet *inline_sheet)
{
	assert(css_select_style(select, n, &unit_ctx, &ctx->media, inline_sheet,
			&select_handler, ctx, &n->sr) == CSS_OK);
	if (n->parent != NULL) {
		css_computed_style *composed;
		assert(css_computed_style_compose(n->parent->sr->styles[0], n->sr->styles[0],
				&unit_ctx, &composed) == CSS_OK);
		css_computed_style_destroy(n->sr->styles[0]);
		n->sr->styles[0] = composed;
	} else unit_ctx.root_style = n->sr->styles[0];
}

static void custom_result(const char *author, const char *user, const char *inlined,
		size_t chunk, char *buffer, size_t *length)
{
	css_select_ctx *select = NULL;
	css_stylesheet *sheet = custom_sheet(author, false, chunk);
	css_stylesheet *us = custom_sheet(user, false, chunk);
	css_stylesheet *is = custom_sheet(inlined, true, chunk);
	assert(css_select_ctx_create(&select) == CSS_OK);
	assert(css_select_ctx_append_sheet(select, us, CSS_ORIGIN_USER, NULL) == CSS_OK);
	assert(css_select_ctx_append_sheet(select, sheet, CSS_ORIGIN_AUTHOR, NULL) == CSS_OK);
	node *root = custom_node("html", NULL);
	node *main = custom_node("main", root);
	node *p = custom_node("p", main);
	line_ctx ctx = { .media = { .type = CSS_MEDIA_SCREEN } };
	unit_ctx.root_style = NULL;
	custom_select(select, root, &ctx, NULL);
	custom_select(select, main, &ctx, NULL);
	custom_select(select, p, &ctx, is);
	size_t capacity = *length;
	dump_computed_style(p->sr->styles[0], buffer, length);
	if (p->sr->styles[CSS_PSEUDO_ELEMENT_BEFORE] != NULL) {
		css_computed_style *pseudo;
		assert(css_computed_style_compose(p->sr->styles[0],
				p->sr->styles[CSS_PSEUDO_ELEMENT_BEFORE], &unit_ctx, &pseudo) == CSS_OK);
		dump_computed_style(pseudo, buffer + capacity - *length, length);
		css_computed_style_destroy(pseudo);
	}
	*length = capacity - *length;
	css__ciuki_computed_row_gap(p->sr->styles[0], &inspected_row, &inspected_row_unit);
	inspected_column_type = css_computed_column_gap(p->sr->styles[0], &inspected_column, &inspected_column_unit);
	/* Repeat selection after node-data invalidation: no stale variable map. */
	assert(css_libcss_node_data_handler(&select_handler, CSS_NODE_MODIFIED,
			&ctx, p, NULL, p->libcss_node_data) == CSS_OK);
	css_select_results_destroy(p->sr);
	p->sr = NULL;
	custom_select(select, p, &ctx, is);
	css_select_ctx_destroy(select);
	css_stylesheet_destroy(is);
	css_stylesheet_destroy(us);
	css_stylesheet_destroy(sheet);
	destroy_tree(root);
	unit_ctx.root_style = NULL;
}

static void custom_check(const char *css, const char *expected,
		const char *user, const char *inlined, const char *expected_inline)
{
	for (size_t chunk = 1; chunk <= 4096; chunk = chunk == 1 ? 7 : 4096) {
		char actual[16384], oracle[16384];
		size_t alen = sizeof(actual), olen = sizeof(oracle);
		custom_result(css, user, inlined, chunk, actual, &alen);
		custom_result(expected, user, expected_inline, chunk, oracle, &olen);
		if (alen != olen || memcmp(actual, oracle, alen) != 0) {
			fprintf(stderr, "custom CSS mismatch (chunk=%zu): %s\n", chunk, css);
			show_differences(alen < olen ? alen : olen, oracle, actual);
			fflush(stdout);
			assert(0);
		}
		if (chunk == 4096) break;
	}
}

static void custom_gap_check(const char *css, int row, int column)
{
	char buffer[16384];
	size_t length = sizeof(buffer);
	custom_result(css, "", "", 1, buffer, &length);
	assert(inspected_row == INTTOFIX(row));
	if (row) assert(inspected_row_unit == CSS_UNIT_PX);
	if (column < 0) assert(inspected_column_type == CSS_COLUMN_GAP_NORMAL);
	else {
		assert(inspected_column_type == CSS_COLUMN_GAP_SET);
		assert(inspected_column == INTTOFIX(column));
		assert(inspected_column_unit == CSS_UNIT_PX);
	}
}

static void custom_oom(void)
{
	css_select_ctx *select = NULL;
	css_stylesheet *sheet = custom_sheet("html{--a:red;--b:var(--a)}p{color:var(--b);gap:12px}", false, 4096);
	assert(css_select_ctx_create(&select) == CSS_OK);
	assert(css_select_ctx_append_sheet(select, sheet, CSS_ORIGIN_AUTHOR, NULL) == CSS_OK);
	node *root = custom_node("html", NULL);
	node *p = custom_node("p", root);
	line_ctx ctx = { .media = { .type = CSS_MEDIA_SCREEN } };
	unit_ctx.root_style = NULL;
	custom_select(select, root, &ctx, NULL);
	unsigned failures = 0;
	for (unsigned failure = 1; failure <= 150; failure++) {
		css_select_results *result = NULL;
		allocation_count = 0;
		fail_allocation = failure;
		css_error error = css_select_style(select, p, &unit_ctx, &ctx.media, NULL,
				&select_handler, &ctx, &result);
		fail_allocation = 0;
		assert(error == CSS_OK || error == CSS_NOMEM);
		if (error == CSS_OK) {
			assert(result != NULL);
			css_color color;
			assert(css_computed_color(result->styles[0], &color) == CSS_COLOR_COLOR && color == 0xffff0000u);
			css_select_results_destroy(result);
			assert(css_libcss_node_data_handler(&select_handler, CSS_NODE_MODIFIED,
					&ctx, p, NULL, p->libcss_node_data) == CSS_OK);
		} else { assert(result == NULL); failures++; }
	}
	assert(failures >= 10);
	css_select_ctx_destroy(select);
	css_stylesheet_destroy(sheet);
	destroy_tree(root);
	unit_ctx.root_style = NULL;
	printf("PASS custom selection: %u allocation failures recovered across 150 injection positions\n", failures);
	fflush(stdout);
}

static void custom_inline_ancestor(void)
{
	css_select_ctx *select = NULL;
	assert(css_select_ctx_create(&select) == CSS_OK);
	css_stylesheet *definition = custom_sheet("--a:red", true, 1);
	css_stylesheet *use = custom_sheet("color:var(--a)", true, 1);
	node *root = custom_node("html", NULL);
	node *main = custom_node("main", root);
	node *p = custom_node("p", main);
	line_ctx ctx = { .media = { .type = CSS_MEDIA_SCREEN } };
	unit_ctx.root_style = NULL;
	custom_select(select, root, &ctx, definition);
	custom_select(select, main, &ctx, NULL);
	custom_select(select, p, &ctx, use);
	css_color color;
	assert(css_computed_color(p->sr->styles[0], &color) == CSS_COLOR_COLOR && color == 0xffff0000u);
	css_select_ctx_destroy(select);
	css_stylesheet_destroy(definition);
	css_stylesheet_destroy(use);
	destroy_tree(root);
	unit_ctx.root_style = NULL;
	puts("PASS inline-only ancestor inheritance across an ordinary element");
}

static void custom_url(void)
{
	css_select_ctx *select = NULL;
	assert(css_select_ctx_create(&select) == CSS_OK);
	css_stylesheet *definition = custom_sheet_at("html{--image:url(tile.png)}", false, 1, "file:///defs/define.css");
	css_stylesheet *use = custom_sheet("p{background-image:var(--image)}", false, 1);
	assert(css_select_ctx_append_sheet(select, definition, CSS_ORIGIN_AUTHOR, NULL) == CSS_OK);
	assert(css_select_ctx_append_sheet(select, use, CSS_ORIGIN_AUTHOR, NULL) == CSS_OK);
	node *root = custom_node("html", NULL), *p = custom_node("p", root);
	line_ctx ctx = { .media = { .type = CSS_MEDIA_SCREEN } };
	unit_ctx.root_style = NULL;
	custom_select(select, root, &ctx, NULL);
	custom_select(select, p, &ctx, NULL);
	lwc_string *url;
	assert(css_computed_background_image(p->sr->styles[0], &url) == CSS_BACKGROUND_IMAGE_IMAGE);

	assert(url != NULL);
	const char *expected = "file:///uses/tile.png";
	assert(lwc_string_length(url) == strlen(expected) && !memcmp(lwc_string_data(url), expected, strlen(expected)));
	css_select_ctx_destroy(select);
	css_stylesheet_destroy(definition);
	css_stylesheet_destroy(use);
	destroy_tree(root);
	unit_ctx.root_style = NULL;
	puts("PASS custom URL resolves in consuming stylesheet");
}

static void custom_bounds(void)
{
	char css[8192];
	size_t used = snprintf(css, sizeof(css), "p{--b0:red;");
	for (unsigned i = 1; i <= 11; i++)
		used += snprintf(css + used, sizeof(css) - used,
				"--b%u:var(--b%u) var(--b%u);", i, i - 1, i - 1);
	snprintf(css + used, sizeof(css) - used, "color:var(--b11,blue)}");
	custom_check(css, "p{color:blue}", "", "", "");
	css_select_ctx *select = NULL;
	used = snprintf(css, sizeof(css), "html{");
	for (unsigned i = 0; i < 129; i++)
		used += snprintf(css + used, sizeof(css) - used, "--v%u:red;", i);
	snprintf(css + used, sizeof(css) - used, "}");
	css_stylesheet *sheet = custom_sheet(css, false, 4096);
	assert(css_select_ctx_create(&select) == CSS_OK);
	assert(css_select_ctx_append_sheet(select, sheet, CSS_ORIGIN_AUTHOR, NULL) == CSS_OK);
	node *root = custom_node("html", NULL);
	line_ctx ctx = { .media = { .type = CSS_MEDIA_SCREEN } };
	css_select_results *result = NULL;
	unit_ctx.root_style = NULL;
	assert(css_select_style(select, root, &unit_ctx, &ctx.media, NULL,
			&select_handler, &ctx, &result) == CSS_NOMEM);
	assert(result == NULL);
	css_select_ctx_destroy(select);
	css_stylesheet_destroy(sheet);
	destroy_tree(root);
	puts("PASS custom variable-count admission and exponential substitution bound");
	fflush(stdout);
}

static void custom_memory(void)
{
	assert(css__custom_memory_used() == 0);
	/* Full admission: absolutely no allocator call on failure. */
	size_t limit = css__custom_memory_limit();
	printf("PASS custom byte budget limit=%zu bytes\n", limit);
	assert(css__custom_reserve(limit));
	css_custom_tokens *tokens = NULL;
	allocation_count = 0;
	assert(css__custom_tokens_create(&tokens) == CSS_NOMEM && tokens == NULL);
	assert(allocation_count == 0 && css__custom_memory_used() == limit);
	css__custom_release(limit);
	/* Vector capacity and retained strings are admitted before append. */
	assert(css__custom_tokens_create(&tokens) == CSS_OK);
	css_token space = { .type = CSS_TOKEN_S };
	for (unsigned i = 0; i < 8; i++) assert(css__custom_tokens_append(tokens, &space) == CSS_OK);
	size_t used = css__custom_memory_used();
	assert(css__custom_reserve(limit - used));
	allocation_count = 0;
	assert(css__custom_tokens_append(tokens, &space) == CSS_NOMEM);
	assert(allocation_count == 0 && tokens->count == 8);
	css__custom_release(limit - used);
	css__custom_tokens_unref(tokens);
	assert(css__custom_memory_used() == 0);

	/* Raw declarations in many sheets all use the SAME budget. */
	css_stylesheet *sheets[32];
	unsigned count = 0;
	size_t held = limit - 8192;
	assert(css__custom_reserve(held));
	css_stylesheet_params params = { .params_version = CSS_STYLESHEET_PARAMS_VERSION_1,
		.level = CSS_LEVEL_21, .charset = "UTF-8", .url = "file:raw-budget", .resolve = custom_resolve };
	css_error error = CSS_OK;
	while (count < 32 && error == CSS_OK) {
		css_stylesheet *sheet;
		assert(css_stylesheet_create(&params, &sheet) == CSS_OK);
		sheets[count++] = sheet;
		const char *css = "*{--a:red red red red red red red red red red red red red red red red}";
		error = css_stylesheet_append_data(sheet, (const uint8_t *)css, strlen(css));
		if (error == CSS_OK || error == CSS_NEEDDATA) error = css_stylesheet_data_done(sheet);
		assert(error == CSS_OK || error == CSS_NOMEM);
		assert(css__custom_memory_used() <= limit);
	}
	assert(error == CSS_NOMEM && count > 1 && count < 32);
	for (unsigned i = 0; i < count; i++) css_stylesheet_destroy(sheets[i]);
	assert(css__custom_memory_used() == held);
	css__custom_release(held);

	/* A universal rule must share its large immutable literal, rather than
	 * retain another token vector per element. Use a tight aggregate budget. */
	char css[4096];
	size_t len = snprintf(css, sizeof(css), "*{--a:");
	for (unsigned i = 0; i < 256; i++) len += snprintf(css + len, sizeof(css) - len, "red ");
	snprintf(css + len, sizeof(css) - len, ";}");
	for (unsigned computed = 0; computed < 2; computed++) {
		if (computed) strcpy(css, "*{--a:red red red red red red red red;--b:var(--a) var(--a) var(--a) var(--a)}");
		css_stylesheet *sheet = custom_sheet(css, false, 4096);
		css_select_ctx *select;
		assert(css_select_ctx_create(&select) == CSS_OK);
		assert(css_select_ctx_append_sheet(select, sheet, CSS_ORIGIN_AUTHOR, NULL) == CSS_OK);
		held = limit - css__custom_memory_used() - 65536;
		assert(css__custom_reserve(held));
		node *root = custom_node("html", NULL);
		line_ctx ctx = { .media = { .type = CSS_MEDIA_SCREEN } };
		unit_ctx.root_style = NULL;
		custom_select(select, root, &ctx, NULL);
		unsigned selected = 0;
		for (; selected < 1024; selected++) {
			node *child = custom_node("p", root);
			css_select_results *result = NULL;
			error = css_select_style(select, child, &unit_ctx, &ctx.media, NULL,
					&select_handler, &ctx, &result);
			assert(css__custom_memory_used() <= limit);
			if (error != CSS_OK) { assert(error == CSS_NOMEM && result == NULL); break; }
			child->sr = result;
		}
		assert(selected < 1024);
		if (!computed) assert(selected >= 32); /* would exhaust on the first copies */
		else assert(selected >= 1);
		/* Node references keep tokens charged after the source sheet dies. */
		css_select_ctx_destroy(select);
		css_stylesheet_destroy(sheet);
		assert(css__custom_memory_used() > held);
		destroy_tree(root);
		unit_ctx.root_style = NULL;
		assert(css__custom_memory_used() == held);
		css__custom_release(held);
		assert(css__custom_memory_used() == 0);
		printf("PASS document-wide custom byte admission: %s selected=%u bounded, cleanup=0\n",
				computed ? "substituted" : "shared literal", selected);
	}
}

static void custom_feature_cache(void)
{
	char rules[32768];
	size_t used = 0;
	for (unsigned i = 0; i < 1000; i++)
		used += snprintf(rules + used, sizeof(rules) - used, ".unused%u{color:red}", i);
	css_stylesheet *sheet = custom_sheet(rules, false, 4096);
	css_select_ctx *select;
	assert(css_select_ctx_create(&select) == CSS_OK);
	assert(css_select_ctx_append_sheet(select, sheet, CSS_ORIGIN_AUTHOR, NULL) == CSS_OK);
	assert(!css__custom_sheet_uses(sheet, 0));
	size_t scans = sheet->custom_feature_scans;
	assert(scans >= 1000);
	node *root = custom_node("html", NULL);
	line_ctx ctx = { .media = { .type = CSS_MEDIA_SCREEN } };
	unit_ctx.root_style = NULL;
	custom_select(select, root, &ctx, NULL);
	for (unsigned i = 0; i < 100; i++) {
		/* Inline parsing/destruction in another element is unrelated to this
		 * import graph and must not force its full scan again. */
		css_stylesheet *inlined = custom_sheet("color:blue", true, 4096);
		custom_select(select, custom_node("p", root), &ctx, inlined);
		css_stylesheet_destroy(inlined);
	}
	assert(sheet->custom_feature_scans == scans);
	css_select_ctx_destroy(select);
	css_stylesheet_destroy(sheet);
	destroy_tree(root);
	unit_ctx.root_style = NULL;

	/* Cache a negative graph, then register a late, nested custom import. */
	css_stylesheet *top = custom_sheet("@import 'middle.css';p{color:var(--x,blue)}", false, 4096);
	css_stylesheet *middle = custom_sheet("@import 'leaf.css';p{color:red}", false, 4096);
	css_stylesheet *leaf = custom_sheet("p{--x:lime}", false, 4096);
	css_stylesheet *ordinary = custom_sheet("@import 'ordinary.css';p{color:red}", false, 4096);
	assert(!css__custom_sheet_uses(ordinary, 0));
	assert(css_stylesheet_register_import(ordinary, middle) == CSS_OK);
	assert(!css__custom_sheet_uses(ordinary, 0));
	scans = ordinary->custom_feature_scans + middle->custom_feature_scans;
	for (unsigned i = 0; i < 100; i++) assert(!css__custom_sheet_uses(ordinary, 0));
	assert(scans == ordinary->custom_feature_scans + middle->custom_feature_scans);
	assert(css_stylesheet_register_import(middle, leaf) == CSS_OK);
	assert(css__custom_sheet_uses(ordinary, 0));
	scans = ordinary->custom_feature_scans + middle->custom_feature_scans;
	for (unsigned i = 0; i < 100; i++) assert(css__custom_sheet_uses(ordinary, 0));
	assert(scans == ordinary->custom_feature_scans + middle->custom_feature_scans);
	assert(css_stylesheet_register_import(top, middle) == CSS_OK);
	assert(css__custom_sheet_uses(top, 0));
	css_stylesheet_destroy(top);
	css_stylesheet_destroy(ordinary);
	css_stylesheet_destroy(middle);
	css_stylesheet_destroy(leaf);
	assert(css__custom_memory_used() == 0);
	puts("PASS feature cache: 1000-rule sheet x 101 selections scans once; nested late imports invalidate");
}

int main(void)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	static const struct { const char *css, *expected; } cases[] = {
		{ ":root{--paper:#fafbf8}p{background:var(--paper)}", "p{background:#fafbf8}" },
		{ "html{--v:red}main{--v:blue}p{color:var(--v)}", "p{color:blue}" },
		{ "html{--v:red!important}main{--v:blue}p{color:var(--v)}", "p{color:blue}" },
		{ "p{color:var(--later)}html{--later:#123456}", "p{color:#123456}" },
		{ "p{flex:0 0}", "p{flex-grow:0;flex-shrink:0;flex-basis:0px}" },
		{ "p{flex:0 0 0}", "p{flex-grow:0;flex-shrink:0;flex-basis:0px}" },
		{ "p{flex:0 0 40px}", "p{flex-grow:0;flex-shrink:0;flex-basis:40px}" },
		{ "p{--a:red;--a:blue;color:var(--a)}", "p{color:blue}" },
		{ "p{--a:red!important;--a:blue;color:var(--a)}", "p{color:red}" },
		{ "p{--A:red;--a:blue;color:var(--A)}", "p{color:red}" },
		{ "p{color:var(--missing,var(--also-missing,red))}", "p{color:red}" },
		{ "p{--a:var(--missing,red);color:var(--a)}", "p{color:red}" },
		{ "html{--a:red;--b:var(--a)}p{--a:blue;color:var(--b)}", "p{color:red}" },
		{ "html{--a:red}p{--a:initial;color:var(--a,blue)}", "p{color:blue}" },
		{ "html{--a:red}p{--a:inherit;color:var(--a)}", "p{color:red}" },
		{ "html{--a:red}p{--a:unset;color:var(--a)}", "p{color:red}" },
		{ "html{--a:red}p{--a:var(--a);color:var(--a,blue)}", "p{color:blue}" },
		{ "p{--a:var(--b);--b:var(--a);color:var(--a,red)}", "p{color:red}" },
		{ "p{--a:var(--b) var(--c);--b:var(--a);--c:var(--b,red);color:var(--c,blue)}", "p{color:blue}" },
		{ "p{--c:var(--b,red);--b:var(--a);--a:var(--b) var(--c);color:var(--c,blue)}", "p{color:blue}" },
		{ "p{--a:var(--b) var(--c);--b:var(--a);--c:var(--b,red);--d:var(--c,lime);color:var(--d)}", "p{color:lime}" },
		{ "p{--a:var(--b,red);--b:var(--a,blue);color:var(--a,green)}", "p{color:green}" },
		{ "p{--a:var(--b,red);--b:var(--a);--c:var(--a,green);color:var(--c)}", "p{color:green}" },
		{ "p{--a:red;--b:var(--a,var(--b));color:var(--b,blue)}", "p{color:blue}" },
		{ "main{color:blue}p{color:red;color:var(--absent)}", "main{color:blue}p{color:unset}" },
		{ "main{color:blue}p{--a:12px;color:red;color:var(--a)}", "main{color:blue}p{color:unset}" },
		{ "p{background:red;background:var(--absent)}", "p{background:unset}" },
		{ "p{--a:red;color:var(--a);color:blue}", "p{color:blue}" },
		{ "p{--a:red;color:blue;color:var(--a)}", "p{color:red}" },
		{ "p{--a:red;color:var(--a)!important;color:blue}", "p{color:red!important}" },
		{ "p{--a:12px 24px;margin:var(--a);margin-left:6px}", "p{margin:12px 24px;margin-left:6px}" },
		{ "p{--a:var(--absent);margin:12px;margin:var(--a)}", "p{margin:unset}" },
		{ "p{--a:;color:var(--a,red)}", "p{color:unset}" },
		{ "p{font-family:var(--absent, serif, monospace)}", "p{font-family:serif,monospace}" },
		{ "p{--a:12;width:var(--a)px}", "p{width:unset}" },
		{ "p{color:red;color:var(bad,blue)}", "p{color:red}" },
		{ "p{color:red;color:var(--a blue)}", "p{color:red}" },
		{ "@property --a{syntax:'<color>';inherits:false;initial-value:blue}html{--a:red}p{color:var(--a)}", "p{color:red}" },
		{ "html{--a:red}@media print{html{--a:blue}}p{color:var(--a)}", "p{color:red}" },
		{ "html{--a:red}@media screen{html{--a:blue}}p{color:var(--a)}", "p{color:blue}" },
		{ "p{--x:red;--url:url(tile.png);color:var(--x);background-image:var(--url)}", "p{color:red;background-image:url(tile.png)}" },
		{ "p{--a:#123456;color:var(--a)}p:before{--a:blue;content:'x';color:var(--a)}", "p{color:#123456}p:before{content:'x';color:blue}" },
	};
	for (unsigned i = 0; i < sizeof(cases)/sizeof(cases[0]); i++) {
		custom_check(cases[i].css, cases[i].expected, "", "", "");
		printf("PASS custom %u\n", i + 1); fflush(stdout);
	}
	custom_check("p{--a:blue;color:var(--a)}", "p{color:red}", "p{--a:red!important}", "", "");
	custom_check("p{--a:blue;--a:revert;color:var(--a)}", "p{color:red}", "p{--a:red}", "", "");
	custom_check("p{--a:blue;color:var(--a)}", "", "", "--a:red;color:var(--a)", "color:red");
	custom_check("p{--a:blue!important;color:var(--a)}", "p{color:blue}", "", "--a:red", "");
	custom_gap_check("p{gap:12px 24px}", 12, 24);
	custom_gap_check("p{gap:12px;row-gap:8px}", 8, 12);
	custom_gap_check("p{row-gap:8px;gap:12px}", 12, 12);
	custom_gap_check("p{gap:12px;column-gap:8px}", 12, 8);
	custom_gap_check("p{--g:12px 24px;gap:var(--g)}", 12, 24);
	custom_gap_check("p{gap:7px;gap:-1px}", 7, 7);
	custom_gap_check("p{gap:7px;gap:calc(3px + 4px)}", 7, 7);
	custom_gap_check("p{gap:7px;gap:var(--absent)}", 0, -1);
	custom_gap_check("main{row-gap:2em;font-size:16px}p{row-gap:inherit;font-size:24px}", 32, -1);
	custom_gap_check("p{gap:normal}", 0, -1);
	custom_gap_check("p{gap:3px;gap:normal 2px}", 0, 2);
	custom_gap_check("p{gap:3px;gap:2px normal}", 2, -1);
	custom_gap_check("p{gap:3px;gap:normal normal}", 0, -1);
	custom_gap_check("p{--g:normal 2px;gap:var(--g)}", 0, 2);
	custom_gap_check("p{--g:2px normal;gap:var(--g)}", 2, -1);
	custom_gap_check("p{gap:3px;gap:normal 2px 4px}", 3, 3);
	custom_gap_check("p{gap:3px;gap:inherit 2px}", 3, 3);
	custom_gap_check("p{gap:3px;gap:2px inherit}", 3, 3);
	custom_gap_check("p{gap:7px!important;row-gap:2px}", 7, 7);
	puts("PASS 19 gap cascade/defaulting/inheritance cases");
	custom_url();
	custom_inline_ancestor();
	custom_oom();
	custom_bounds();
	custom_memory();
	custom_feature_cache();
	assert(css__custom_memory_used() == 0);
	lwc_iterate_strings(printing_lwc_iterator, NULL);
	assert(!fail_because_lwc_leaked);
	puts("PASS CSS variables: cascade, inheritance, fallback, cycles, invalid values, shorthand, media, inline, cleanup");
	fflush(stdout);
	return 0;
}
