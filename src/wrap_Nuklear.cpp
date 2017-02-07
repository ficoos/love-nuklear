/*
 * LOVE-Nuklear - MIT licensed; no warranty implied; use at your own risk.
 * authored from 2015-2016 by Micha Mettke
 * adapted to LOVE in 2016 by Kevin Harrison
 */

#include "wrap_Nuklear.h"

#include <stdio.h>
#include <string.h>
#include <common/runtime.h>

#include <modules/graphics/Graphics.h>

#include "Nuklear.h"

#define NK_IMPLEMENTATION
#define NK_INCLUDE_FIXED_TYPES
#define NK_INCLUDE_STANDARD_IO
#define NK_INCLUDE_STANDARD_VARARGS
#define NK_INCLUDE_DEFAULT_ALLOCATOR
#define NK_PRIVATE
#define NK_BUTTON_BEHAVIOR_STACK_SIZE 32
#define NK_FONT_STACK_SIZE 32
#define NK_STYLE_ITEM_STACK_SIZE 256
#define NK_FLOAT_STACK_SIZE 256
#define NK_VECTOR_STACK_SIZE 128
#define NK_FLAGS_STACK_SIZE 64
#define NK_COLOR_STACK_SIZE 256
#include "nuklear/nuklear.h"

/*
 * ===============================================================
 *
 *                          INTERNAL
 *
 * ===============================================================
 */

#define NK_LOVE_MAX_POINTS 1024
#define NK_LOVE_EDIT_BUFFER_LEN (1024 * 1024)
#define NK_LOVE_COMBOBOX_MAX_ITEMS 1024
#define NK_LOVE_MAX_FONTS 1024
#define NK_LOVE_MAX_RATIOS 1024

static lua_State *L;
static struct nk_context context;
static struct nk_user_font *fonts;
static int font_count;
static char *edit_buffer;
static const char **combobox_items;
static struct nk_cursor cursors[NK_CURSOR_COUNT];
static float *floats;
static int layout_ratio_count;

static love::graphics::Graphics *lg;

static void nk_love_set_color(struct nk_color col)
{
	lg->setColor(love::graphics::Colorf(col.r / 255.0, col.g / 255.0, col.b / 255.0, col.a / 255.0));
}

static void nk_love_configureGraphics(int line_thickness, struct nk_color col)
{
	lg->setLineWidth(line_thickness);
	nk_love_set_color(col);
}

static void nk_love_getGraphics(float *line_thickness, struct nk_color *color)
{
	*line_thickness = lg->getLineWidth();
	auto love_color = lg->getColor();
	color->r = love_color.r;
	color->g = love_color.g;
	color->b = love_color.b;
	color->a = love_color.a;
}

static void nk_love_scissor(int x, int y, int w, int h)
{
	love::Rect rect;
	rect.x = x;
	rect.y = y;
	rect.w = w;
	rect.h = h;
	lg->setScissor(rect);
}

static void nk_love_draw_line(int x0, int y0, int x1, int y1,
	int line_thickness, struct nk_color col)
{
	nk_love_configureGraphics(line_thickness, col);
	float coords[] = {(float) x0, (float) y1, (float) x1, (float) y1};
	lg->polyline(coords, 4);
}

static void nk_love_draw_rect(int x, int y, unsigned int w,
	unsigned int h, unsigned int r, int line_thickness,
	struct nk_color col)
{
	nk_love_configureGraphics(line_thickness, col);
	love::graphics::Graphics::DrawMode mode;
	if (line_thickness >= 0) {
		mode = love::graphics::Graphics::DrawMode::DRAW_LINE;
	} else {
		mode = love::graphics::Graphics::DrawMode::DRAW_FILL;
	}
	lg->rectangle(
		mode,
		(float) x,
		(float) y,
		(float) w,
		(float) h,
		(float) r,
		(float) r);
}

static void nk_love_draw_triangle(int x0, int y0, int x1, int y1,
	int x2, int y2,	int line_thickness, struct nk_color col)
{
	nk_love_configureGraphics(line_thickness, col);
	love::graphics::Graphics::DrawMode mode;
	if (line_thickness >= 0) {
		mode = love::graphics::Graphics::DrawMode::DRAW_LINE;
	} else {
		mode = love::graphics::Graphics::DrawMode::DRAW_FILL;
	}
	float coords[] = { (float) x0, (float) y0, (float) x1, (float) y1, (float) x2, (float) y2};
	lg->polygon(mode, coords, 6);
}

static void nk_love_draw_polygon(const struct nk_vec2i *pnts, int count,
	int line_thickness, struct nk_color col)
{
	nk_love_configureGraphics(line_thickness, col);
	love::graphics::Graphics::DrawMode mode;
	if (line_thickness >= 0) {
		mode = love::graphics::Graphics::DrawMode::DRAW_LINE;
	} else {
		mode = love::graphics::Graphics::DrawMode::DRAW_FILL;
	}
	float coords[NK_LOVE_MAX_POINTS*2];
	int i;
	for (i = 0; (i < count) && (i < NK_LOVE_MAX_POINTS); ++i) {
		coords[2*i] = (float) pnts[i].x;
		coords[2*i + 1] = (float) pnts[i].y;
	}
	lg->polygon(mode, coords, i);
}

static void nk_love_draw_polyline(const struct nk_vec2i *pnts,
	int count, int line_thickness, struct nk_color col)
{
	nk_love_configureGraphics(line_thickness, col);
	float coords[NK_LOVE_MAX_POINTS*2];
	int i;
	for (i = 0; (i < count) && (i < NK_LOVE_MAX_POINTS); ++i) {
		coords[2*i] = (float) pnts[i].x;
		coords[2*i + 1] = (float) pnts[i].y;
	}
	lg->polyline(coords, i);
}

static void nk_love_draw_circle(int x, int y, unsigned int w,
	unsigned int h, int line_thickness, struct nk_color col)
{
	nk_love_configureGraphics(line_thickness, col);
	love::graphics::Graphics::DrawMode mode;
	if (line_thickness >= 0) {
		mode = love::graphics::Graphics::DrawMode::DRAW_LINE;
	} else {
		mode = love::graphics::Graphics::DrawMode::DRAW_FILL;
	}
	lg->ellipse(mode, x + w/2, y + h/2, w/2, h/2);
}

static void nk_love_draw_curve(struct nk_vec2i p1, struct nk_vec2i p2,
	struct nk_vec2i p3, struct nk_vec2i p4, unsigned int num_segments,
	int line_thickness, struct nk_color col)
{
	unsigned int i_step;
	float t_step;
	struct nk_vec2i last = p1;

	if (num_segments < 1) {
		num_segments = 1;
	}
	t_step = 1.0f/(float)num_segments;
	nk_love_configureGraphics(line_thickness, col);
	float coords[num_segments * 2];
	for (i_step = 1; i_step <= num_segments; ++i_step) {
		float t = t_step * (float)i_step;
		float u = 1.0f - t;
		float w1 = u*u*u;
		float w2 = 3*u*u*t;
		float w3 = 3*u*t*t;
		float w4 = t * t *t;
		float x = w1 * p1.x + w2 * p2.x + w3 * p3.x + w4 * p4.x;
		float y = w1 * p1.y + w2 * p2.y + w3 * p3.y + w4 * p4.y;
		coords[2 * (i_step - 1)] = x;
		coords[2 * (i_step - 1) + 1] = y;
	}
	lg->polyline(coords, num_segments * 2);
}

static float nk_love_get_text_width(nk_handle handle, float height,
	const char *text, int len)
{
	lua_getfield(L, LUA_REGISTRYINDEX, "nuklear");
	lua_getfield(L, -1, "font");
	lua_rawgeti(L, -1, handle.id);
	love::graphics::Font *font = luax_checktype<love::graphics::Font>(L, -1);
	lua_pop(L, 3);
	return font->getWidth(std::string(text, len));
}

static void nk_love_draw_text(int fontref, struct nk_color cbg,
	struct nk_color cfg, int x, int y, unsigned int w, unsigned int h,
	float height, int len, const char *text)
{
	//nk_love_set_color(cbg);
	//lg->rectangle(love::graphics::Graphics::DrawMode::DRAW_FILL, x, y, w, height);
	nk_love_set_color(cfg);

	lua_getfield(L, LUA_REGISTRYINDEX, "nuklear");
	lua_getfield(L, -1, "font");
	lua_rawgeti(L, -1, fontref);
	auto font = luax_checktype<love::graphics::Font>(L, -1);
	lua_pop(L, 3);

	lg->setFont(font);
	std::vector<love::graphics::Font::ColoredString> str;
	auto transform = lg->getTransform();
	transform.translate(x, y);
	str.push_back({text, lg->getColor()});
	lg->print(str, transform);
}

static void interpolate_color(struct nk_color c1, struct nk_color c2,
	struct nk_color *result, float fraction)
{
	float r = c1.r + (c2.r - c1.r) * fraction;
	float g = c1.g + (c2.g - c1.g) * fraction;
	float b = c1.b + (c2.b - c1.b) * fraction;
	float a = c1.a + (c2.a - c1.a) * fraction;

	result->r = (nk_byte)NK_CLAMP(0, r, 255);
	result->g = (nk_byte)NK_CLAMP(0, g, 255);
	result->b = (nk_byte)NK_CLAMP(0, b, 255);
	result->a = (nk_byte)NK_CLAMP(0, a, 255);
}

static void nk_love_draw_rect_multi_color(int x, int y, unsigned int w,
	unsigned int h, struct nk_color left, struct nk_color top,
	struct nk_color right, struct nk_color bottom)
{
	lg->push(love::graphics::Graphics::StackType::STACK_ALL);
	nk_love_set_color({255, 255, 255, 255});
	lg->setPointSize(1);

	struct nk_color X1, X2, Y;
	float fraction_x, fraction_y;
	int i,j;

	float points[h * w * 2];
	love::graphics::Colorf colors [h * w];
	int n = 0;
	for (j = 0; j < h; j++) {
		fraction_y = ((float)j) / h;
		for (i = 0; i < w; i++) {
			fraction_x = ((float)i) / w;
			interpolate_color(left, top, &X1, fraction_x);
			interpolate_color(right, bottom, &X2, fraction_x);
			interpolate_color(X1, X2, &Y, fraction_y);
			points[2 * n] = x + i;
			points[2 * n + 1] = y + j;
			colors[n].r = Y.r / 255.0;
			colors[n].g = Y.g / 255.0;
			colors[n].b = Y.b / 255.0;
			colors[n].a = Y.a / 255.0;
			n++;
		}
	}

	lg->points(points, colors, n);

	lg->pop();
}

static void nk_love_clear(struct nk_color col)
{
	love::graphics::Colorf lcol(col.r/255.0, col.g/255.0, col.b/255.0, col.a/255.0);
	lg->clear(lcol);
}

static void nk_love_blit()
{
	lg->present(0);
}

static void nk_love_draw_image(int x, int y, unsigned int w, unsigned int h,
	struct nk_image image, struct nk_color color)
{
	nk_love_configureGraphics(-1, color);
	lua_getfield(L, LUA_REGISTRYINDEX, "nuklear");
	lua_getfield(L, -1, "image");
	lua_rawgeti(L, -1, image.handle.id);
	lua_getfield(L, -5, "newQuad");
	lua_pushnumber(L, image.region[0]);
	lua_pushnumber(L, image.region[1]);
	lua_pushnumber(L, image.region[2]);
	lua_pushnumber(L, image.region[3]);
	lua_pushnumber(L, image.w);
	lua_pushnumber(L, image.h);
	lua_call(L, 6, 1);
	lua_replace(L, -3);
	lua_replace(L, -3);
	lua_pushnumber(L, x);
	lua_pushnumber(L, y);
	lua_pushnumber(L, 0);
	lua_pushnumber(L, (double) w / image.region[2]);
	lua_pushnumber(L, (double) h / image.region[3]);
	lua_call(L, 7, 0);
	lua_pop(L, 1);
}

static void nk_love_draw_arc(int cx, int cy, unsigned int r,
	int line_thickness, float a1, float a2, struct nk_color color)
{
	nk_love_configureGraphics(line_thickness, color);
	lua_getfield(L, -1, "arc");
	if (line_thickness >= 0) {
		lua_pushstring(L, "line");
	} else {
		lua_pushstring(L, "fill");
	}
	lua_pushnumber(L, cx);
	lua_pushnumber(L, cy);
	lua_pushnumber(L, r);
	lua_pushnumber(L, a1);
	lua_pushnumber(L, a2);
	lua_call(L, 6, 0);
	lua_pop(L, 1);
}

static void nk_love_clipbard_paste(nk_handle usr, struct nk_text_edit *edit)
{
	(void)usr;
	lua_getglobal(L, "love");
	lua_getfield(L, -1, "system");
	lua_getfield(L, -1, "getClipboardText");
	lua_call(L, 0, 1);
	const char *text = lua_tostring(L, -1);
	if (text) nk_textedit_paste(edit, text, nk_strlen(text));
	lua_pop(L, 3);
}

static void nk_love_clipbard_copy(nk_handle usr, const char *text, int len)
{
	(void)usr;
	char *str = 0;
	if (!len) return;
	str = (char*)malloc((size_t)len+1);
	if (!str) return;
	memcpy(str, text, (size_t)len);
	str[len] = '\0';
	lua_getglobal(L, "love");
	lua_getfield(L, -1, "system");
	lua_getfield(L, -1, "setClipboardText");
	lua_pushstring(L, str);
	free(str);
	lua_call(L, 1, 0);
	lua_pop(L, 2);
}

static int nk_love_is_active(struct nk_context *ctx)
{
	struct nk_window *iter;
	NK_ASSERT(ctx);
	if (!ctx) return 0;
	iter = ctx->begin;
	while (iter) {
		/* check if window is being hovered */
		if (iter->flags & NK_WINDOW_MINIMIZED) {
			struct nk_rect header = iter->bounds;
			header.h = ctx->style.font->height + 2 * ctx->style.window.header.padding.y;
			if (nk_input_is_mouse_hovering_rect(&ctx->input, header))
				return 1;
		} else if (nk_input_is_mouse_hovering_rect(&ctx->input, iter->bounds)) {
			return 1;
		}
		/* check if window popup is being hovered */
		if (iter->popup.active && iter->popup.win && nk_input_is_mouse_hovering_rect(&ctx->input, iter->popup.win->bounds))
			return 1;
		if (iter->edit.active & NK_EDIT_ACTIVE)
			return 1;
		iter = iter->next;
	}
	return 0;
}

static int nk_love_keyevent(const char *key, const char *scancode,
	int isrepeat, int down)
{
	lua_getglobal(L, "love");
	lua_getfield(L, -1, "keyboard");
	lua_getfield(L, -1, "isScancodeDown");
	lua_pushstring(L, "lctrl");
	lua_call(L, 1, 1);
	int lctrl = lua_toboolean(L, -1);
	lua_pop(L, 3);

	if (!strcmp(key, "rshift") || !strcmp(key, "lshift"))
		nk_input_key(&context, NK_KEY_SHIFT, down);
	else if (!strcmp(key, "delete"))
		nk_input_key(&context, NK_KEY_DEL, down);
	else if (!strcmp(key, "return"))
		nk_input_key(&context, NK_KEY_ENTER, down);
	else if (!strcmp(key, "tab"))
		nk_input_key(&context, NK_KEY_TAB, down);
	else if (!strcmp(key, "backspace"))
		nk_input_key(&context, NK_KEY_BACKSPACE, down);
	else if (!strcmp(key, "home")) {
		nk_input_key(&context, NK_KEY_TEXT_LINE_START, down);
	} else if (!strcmp(key, "end")) {
		nk_input_key(&context, NK_KEY_TEXT_LINE_END, down);
	} else if (!strcmp(key, "pagedown")) {
		nk_input_key(&context, NK_KEY_SCROLL_DOWN, down);
	} else if (!strcmp(key, "pageup")) {
		nk_input_key(&context, NK_KEY_SCROLL_UP, down);
	} else if (!strcmp(key, "z"))
		nk_input_key(&context, NK_KEY_TEXT_UNDO, down && lctrl);
	else if (!strcmp(key, "r"))
		nk_input_key(&context, NK_KEY_TEXT_REDO, down && lctrl);
	else if (!strcmp(key, "c"))
		nk_input_key(&context, NK_KEY_COPY, down && lctrl);
	else if (!strcmp(key, "v"))
		nk_input_key(&context, NK_KEY_PASTE, down && lctrl);
	else if (!strcmp(key, "x"))
		nk_input_key(&context, NK_KEY_CUT, down && lctrl);
	else if (!strcmp(key, "b"))
		nk_input_key(&context, NK_KEY_TEXT_LINE_START, down && lctrl);
	else if (!strcmp(key, "e"))
		nk_input_key(&context, NK_KEY_TEXT_LINE_END, down && lctrl);
	else if (!strcmp(key, "left")) {
		if (lctrl)
			nk_input_key(&context, NK_KEY_TEXT_WORD_LEFT, down);
		else
			nk_input_key(&context, NK_KEY_LEFT, down);
	} else if (!strcmp(key, "right")) {
		if (lctrl)
			nk_input_key(&context, NK_KEY_TEXT_WORD_RIGHT, down);
		else
			nk_input_key(&context, NK_KEY_RIGHT, down);
	} else if (!strcmp(key, "up"))
		nk_input_key(&context, NK_KEY_UP, down);
	else if (!strcmp(key, "down"))
		nk_input_key(&context, NK_KEY_DOWN, down);
	else
		return 0;
	return nk_love_is_active(&context);
}

static int nk_love_clickevent(int x, int y, int button, int istouch, int down)
{
	if (button == 1)
		nk_input_button(&context, NK_BUTTON_LEFT, x, y, down);
	else if (button == 3)
		nk_input_button(&context, NK_BUTTON_MIDDLE, x, y, down);
	else if (button == 2)
		nk_input_button(&context, NK_BUTTON_RIGHT, x, y, down);
	else
		return 0;
	return nk_window_is_any_hovered(&context);
}

static int nk_love_mousemoved_event(int x, int y, int dx, int dy, int istouch)
{
	nk_input_motion(&context, x, y);
	return nk_window_is_any_hovered(&context);
}

static int nk_love_textinput_event(const char *text)
{
	nk_rune rune;
	nk_utf_decode(text, &rune, strlen(text));
	nk_input_unicode(&context, rune);
	return nk_love_is_active(&context);
}

static int nk_love_wheelmoved_event(int x, int y)
{
	nk_input_scroll(&context,(float)y);
	return nk_window_is_any_hovered(&context);
}

/*
 * ===============================================================
 *
 *                          WRAPPER
 *
 * ===============================================================
 */

static int nk_love_is_type(int index, const char *type)
{
	if (index < 0)
		index += lua_gettop(L) + 1;
	if (lua_isuserdata(L, index)) {
		lua_getfield(L, index, "typeOf");
		if (lua_isfunction(L, -1)) {
			lua_pushvalue(L, index);
			lua_pushstring(L, type);
			lua_call(L, 2, 1);
			if (lua_isboolean(L, -1)) {
				int is_type = lua_toboolean(L, -1);
				lua_pop(L, 1);
				return is_type;
			}
		}
	}
	return 0;
}

static void nk_love_checkFont(int index, struct nk_user_font *font)
{
	if (index < 0)
		index += lua_gettop(L) + 1;
	if (!nk_love_is_type(index, "Font"))
		luaL_typerror(L, index, "Font");
	lua_getfield(L, LUA_REGISTRYINDEX, "nuklear");
	lua_getfield(L, -1, "font");
	lua_pushvalue(L, index);
	int ref = luaL_ref(L, -2);
	lua_getfield(L, index, "getHeight");
	lua_pushvalue(L, index);
	lua_call(L, 1, 1);
	float height = lua_tonumber(L, -1);
	font->userdata = nk_handle_id(ref);
	font->height = height;
	font->width = nk_love_get_text_width;
	lua_pop(L, 3);
}

static void nk_love_checkImage(int index, struct nk_image *image)
{
	if (index < 0)
		index += lua_gettop(L) + 1;
	if (!nk_love_is_type(index, "Image"))
		luaL_typerror(L, index, "Image");
	lua_getfield(L, LUA_REGISTRYINDEX, "nuklear");
	lua_getfield(L, -1, "image");
	lua_pushvalue(L, index);
	int ref = luaL_ref(L, -2);
	lua_getfield(L, index, "getDimensions");
	lua_pushvalue(L, index);
	lua_call(L, 1, 2);
	int width = lua_tointeger(L, -2);
	int height = lua_tointeger(L, -1);
	image->handle = nk_handle_id(ref);
	image->w = width;
	image->h = height;
	image->region[0] = 0;
	image->region[1] = 0;
	image->region[2] = width;
	image->region[3] = height;
	lua_pop(L, 4);
}

static int nk_love_is_hex(char c)
{
	return (c >= '0' && c <= '9')
			|| (c >= 'a' && c <= 'f')
			|| (c >= 'A' && c <= 'F');
}

static int nk_love_is_color(int index)
{
	if (index < 0)
		index += lua_gettop(L) + 1;
	if (lua_isstring(L, index)) {
		size_t len;
		const char *color_string = lua_tolstring(L, index, &len);
		if ((len == 7 || len == 9) && color_string[0] == '#') {
			int i;
			for (i = 1; i < len; ++i) {
				if (!nk_love_is_hex(color_string[i]))
					return 0;
			}
			return 1;
		}
	}
	return 0;
}

static struct nk_color nk_love_checkcolor(int index)
{
	if (index < 0)
		index += lua_gettop(L) + 1;
	if (!nk_love_is_color(index)) {
		if (lua_isstring(L, index)){
			const char *msg = lua_pushfstring(L, "bad color string '%s'", lua_tostring(L, index));
			luaL_argerror(L, index, msg);
		} else {
			luaL_typerror(L, index, "color string");
		}
	}
	size_t len;
	const char *color_string = lua_tolstring(L, index, &len);
	int r, g, b, a = 255;
	sscanf(color_string, "#%02x%02x%02x", &r, &g, &b);
	if (len == 9) {
		sscanf(color_string + 7, "%02x", &a);
	}
	struct nk_color color = {(nk_byte) r, (nk_byte) g, (nk_byte) b, (nk_byte) a};
	return color;
}

static nk_flags nk_love_parse_window_flags(int flags_begin) {
	int argc = lua_gettop(L);
	nk_flags flags = NK_WINDOW_NO_SCROLLBAR;
	int i;
	for (i = flags_begin; i <= argc; ++i) {
		const char *flag = luaL_checkstring(L, i);
		if (!strcmp(flag, "border"))
			flags |= NK_WINDOW_BORDER;
		else if (!strcmp(flag, "movable"))
			flags |= NK_WINDOW_MOVABLE;
		else if (!strcmp(flag, "scalable"))
			flags |= NK_WINDOW_SCALABLE;
		else if (!strcmp(flag, "closable"))
			flags |= NK_WINDOW_CLOSABLE;
		else if (!strcmp(flag, "minimizable"))
			flags |= NK_WINDOW_MINIMIZABLE;
		else if (!strcmp(flag, "scrollbar"))
			flags &= ~NK_WINDOW_NO_SCROLLBAR;
		else if (!strcmp(flag, "title"))
			flags |= NK_WINDOW_TITLE;
		else if (!strcmp(flag, "scroll auto hide"))
			flags |= NK_WINDOW_SCROLL_AUTO_HIDE;
		else if (!strcmp(flag, "background"))
			flags |= NK_WINDOW_BACKGROUND;
		else {
			const char *msg = lua_pushfstring(L, "unrecognized window flag '%s'", flag);
			luaL_argerror(L, i, msg);
		}
	}
	return flags;
}

static enum nk_symbol_type nk_love_checksymbol(int index)
{
	if (index < 0)
		index += lua_gettop(L) + 1;
	const char *s = luaL_checkstring(L, index);
	if (!strcmp(s, "none")) {
		return NK_SYMBOL_NONE;
	} else if (!strcmp(s, "x")) {
		return NK_SYMBOL_X;
	} else if (!strcmp(s, "underscore")) {
		return NK_SYMBOL_UNDERSCORE;
	} else if (!strcmp(s, "circle solid")) {
		return NK_SYMBOL_CIRCLE_SOLID;
	} else if (!strcmp(s, "circle outline")) {
		return NK_SYMBOL_CIRCLE_OUTLINE;
	} else if (!strcmp(s, "rect solid")) {
		return NK_SYMBOL_RECT_SOLID;
	} else if (!strcmp(s, "rect outline")) {
		return NK_SYMBOL_RECT_OUTLINE;
	} else if (!strcmp(s, "triangle up")) {
		return NK_SYMBOL_TRIANGLE_UP;
	} else if (!strcmp(s, "triangle down")) {
		return NK_SYMBOL_TRIANGLE_DOWN;
	} else if (!strcmp(s, "triangle left")) {
		return NK_SYMBOL_TRIANGLE_LEFT;
	} else if (!strcmp(s, "triangle right")) {
		return NK_SYMBOL_TRIANGLE_RIGHT;
	} else if (!strcmp(s, "plus")) {
		return NK_SYMBOL_PLUS;
	} else if (!strcmp(s, "minus")) {
		return NK_SYMBOL_MINUS;
	} else if (!strcmp(s, "max")) {
		return NK_SYMBOL_MAX;
	} else {
		const char *msg = lua_pushfstring(L, "unrecognized symbol type '%s'", s);
		luaL_argerror(L, index, msg);
	}
}

static nk_flags nk_love_checkalign(int index)
{
	if (index < 0)
		index += lua_gettop(L) + 1;
	const char *s = luaL_checkstring(L, index);
	if (!strcmp(s, "left")) {
		return NK_TEXT_LEFT;
	} else if (!strcmp(s, "centered")) {
		return NK_TEXT_CENTERED;
	} else if (!strcmp(s, "right")) {
		return NK_TEXT_RIGHT;
	} else if (!strcmp(s, "top left")) {
		return NK_TEXT_ALIGN_TOP | NK_TEXT_ALIGN_LEFT;
	} else if (!strcmp(s, "top centered")) {
		return NK_TEXT_ALIGN_TOP | NK_TEXT_ALIGN_CENTERED;
	} else if (!strcmp(s, "top right")) {
		return NK_TEXT_ALIGN_TOP | NK_TEXT_ALIGN_RIGHT;
	} else if (!strcmp(s, "bottom left")) {
		return NK_TEXT_ALIGN_BOTTOM | NK_TEXT_ALIGN_LEFT;
	} else if (!strcmp(s, "bottom centered")) {
		return NK_TEXT_ALIGN_BOTTOM | NK_TEXT_ALIGN_CENTERED;
	} else if (!strcmp(s, "bottom right")) {
		return NK_TEXT_ALIGN_BOTTOM | NK_TEXT_ALIGN_RIGHT;
	} else {
		const char *msg = lua_pushfstring(L, "unrecognized alignment '%s'", s);
		luaL_argerror(L, index, msg);
	}
}

static enum nk_buttons nk_love_checkbutton(int index)
{
	if (index < 0)
		index += lua_gettop(L) + 1;
	const char *s = luaL_checkstring(L, index);
	if (!strcmp(s, "left")) {
		return NK_BUTTON_LEFT;
	} else if (!strcmp(s, "right")) {
		return NK_BUTTON_RIGHT;
	} else if (!strcmp(s, "middle")) {
		return NK_BUTTON_MIDDLE;
	} else {
		const char *msg = lua_pushfstring(L, "unrecognized mouse button '%s'", s);
		luaL_argerror(L, index, msg);
	}
}

static enum nk_layout_format nk_love_checkformat(int index) {
	if (index < 0)
		index += lua_gettop(L) + 1;
	const char *type = luaL_checkstring(L, index);
	if (!strcmp(type, "dynamic")) {
		return NK_DYNAMIC;
	} else if (!strcmp(type, "static")) {
		return NK_STATIC;
	} else {
		const char *msg = lua_pushfstring(L, "unrecognized layout format '%s'", type);
		luaL_argerror(L, index, msg);
	}
}

static enum nk_tree_type nk_love_checktree(int index) {
	if (index < 0)
		index += lua_gettop(L) + 1;
	const char *type_string = luaL_checkstring(L, index);
	if (!strcmp(type_string, "node")) {
		return NK_TREE_NODE;
	} else if (!strcmp(type_string, "tab")) {
		return NK_TREE_TAB;
	} else {
		const char *msg = lua_pushfstring(L, "unrecognized tree type '%s'", type_string);
		luaL_argerror(L, index, msg);
	}
}

static enum nk_collapse_states nk_love_checkstate(int index) {
	if (index < 0)
		index += lua_gettop(L) + 1;
	const char *state_string = luaL_checkstring(L, index);
	if (!strcmp(state_string, "collapsed")) {
		return NK_MINIMIZED;
	} else if (!strcmp(state_string, "expanded")) {
		return NK_MAXIMIZED;
	} else {
		const char *msg = lua_pushfstring(L, "unrecognized tree state '%s'", state_string);
		luaL_argerror(L, index, msg);
	}
}

static enum nk_button_behavior nk_love_checkbehavior(int index) {
	if (index < 0)
		index += lua_gettop(L) + 1;
	const char *behavior_string = luaL_checkstring(L, index);
	if (!strcmp(behavior_string, "default"))
		return NK_BUTTON_DEFAULT;
	else if (!strcmp(behavior_string, "repeater"))
		return NK_BUTTON_REPEATER;
	else {
		const char *msg = lua_pushfstring(L, "unrecognized button behavior '%s'", behavior_string);
		luaL_argerror(L, index, msg);
	}
}

static enum nk_color_format nk_love_checkcolorformat(int index) {
	if (index < 0)
		index += lua_gettop(L) + 1;
	const char *format_string = luaL_checkstring(L, index);
	if (!strcmp(format_string, "RGB")) {
		return NK_RGB;
	} else if (!strcmp(format_string, "RGBA")) {
	 	return NK_RGBA;
	} else {
		const char *msg = lua_pushfstring(L, "unrecognized color format '%s'", format_string);
		luaL_argerror(L, index, msg);
	}
}

static nk_flags nk_love_checkedittype(int index) {
	if (index < 0)
		index += lua_gettop(L) + 1;
	const char *type_string = luaL_checkstring(L, index);
	if (!strcmp(type_string, "simple")) {
		return NK_EDIT_SIMPLE;
	} else if (!strcmp(type_string, "field")) {
		return NK_EDIT_FIELD;
	} else if (!strcmp(type_string, "box")) {
		return NK_EDIT_BOX;
	} else {
	 	const char *msg = lua_pushfstring(L, "unrecognized edit type '%s'", type_string);
	 	luaL_argerror(L, index, msg);
 }
}

static enum nk_popup_type nk_love_checkpopup(int index) {
	if (index < 0)
		index += lua_gettop(L) + 1;
	const char *popup_type = luaL_checkstring(L, index);
	if (!strcmp(popup_type, "dynamic")) {
		return NK_POPUP_DYNAMIC;
	} else if (!strcmp(popup_type, "static")) {
		return NK_POPUP_STATIC;
	} else {
		const char *msg = lua_pushfstring(L, "unrecognized popup type '%s'", popup_type);
	 	luaL_argerror(L, index, msg);
	}
}

enum nk_love_draw_mode {NK_LOVE_FILL, NK_LOVE_LINE};

static enum nk_love_draw_mode nk_love_checkdraw(int index) {
	if (index < 0)
		index += lua_gettop(L) + 1;
	const char *mode = luaL_checkstring(L, index);
	if (!strcmp(mode, "fill")) {
		return NK_LOVE_FILL;
	} else if (!strcmp(mode, "line")) {
		return NK_LOVE_LINE;
	} else {
		const char *msg = lua_pushfstring(L, "unrecognized draw mode '%s'", mode);
	 	luaL_argerror(L, index, msg);
	}
}

static int nk_love_checkboolean(lua_State *L, int index)
{
	if (index < 0)
		index += lua_gettop(L) + 1;
	luaL_checktype(L, index, LUA_TBOOLEAN);
	return lua_toboolean(L, index);
}

static void nk_love_assert(int pass, const char *msg) {
	if (!pass) {
		lua_Debug ar;
		ar.name = NULL;
		if (lua_getstack(L, 0, &ar))
			lua_getinfo(L, "n", &ar);
		if (ar.name == NULL)
			ar.name = "?";
		luaL_error(L, msg, ar.name);
	}
}

static void nk_love_assert_argc(int pass) {
	nk_love_assert(pass, "wrong number of arguments to '%s'");
}

static void nk_love_assert_alloc(void *mem) {
	nk_love_assert(mem != NULL, "out of memory in '%s'");
}

static void *nk_love_malloc(size_t size) {
	void *mem = malloc(size);
	nk_love_assert_alloc(mem);
	return mem;
}

static int nk_love_init(lua_State *luaState)
{
	lg = love::Module::getInstance<love::graphics::Graphics>(love::Module::M_GRAPHICS);
	L = luaState;
	nk_love_assert_argc(lua_gettop(L) == 0);
	lua_newtable(L);
	lua_pushvalue(L, -1);
	lua_setfield(L, LUA_REGISTRYINDEX, "nuklear");
	lua_newtable(L);
	lua_setfield(L, -2, "font");
	lua_newtable(L);
	lua_setfield(L, -2, "image");
	lua_newtable(L);
	lua_setfield(L, -2, "stack");
	fonts = (nk_user_font*) nk_love_malloc(sizeof(struct nk_user_font) * NK_LOVE_MAX_FONTS);
	lua_getglobal(L, "love");
	nk_love_assert(lua_istable(L, -1), "LOVE-Nuklear requires LOVE environment");
	lua_getfield(L, -1, "graphics");
	lua_getfield(L, -1, "getFont");
	lua_call(L, 0, 1);
	nk_love_checkFont(-1, &fonts[0]);
	nk_init_default(&context, &fonts[0]);
	font_count = 1;
	context.clip.copy = nk_love_clipbard_copy;
	context.clip.paste = nk_love_clipbard_paste;
	context.clip.userdata = nk_handle_ptr(0);
	edit_buffer = (char*) nk_love_malloc(NK_LOVE_EDIT_BUFFER_LEN);
	combobox_items = (const char **) nk_love_malloc(sizeof(char*) * NK_LOVE_COMBOBOX_MAX_ITEMS);
	floats = (float*) nk_love_malloc(sizeof(float) * NK_MAX(NK_LOVE_MAX_RATIOS, NK_LOVE_MAX_POINTS * 2));
	return 0;
}

static int nk_love_shutdown(lua_State *luaState)
{
	nk_love_assert_argc(lua_gettop(L) == 0);
	nk_free(&context);
	lua_pushnil(L);
	lua_setfield(L, LUA_REGISTRYINDEX, "nuklear");
	L = NULL;
	free(fonts);
	fonts = NULL;
	free(edit_buffer);
	edit_buffer = NULL;
	free(combobox_items);
	combobox_items = NULL;
	free(floats);
	floats = NULL;
	return 0;
}

static int nk_love_keypressed(lua_State *L)
{
	nk_love_assert_argc(lua_gettop(L) == 3);
	const char *key = luaL_checkstring(L, 1);
	const char *scancode = luaL_checkstring(L, 2);
	int isrepeat = nk_love_checkboolean(L, 3);
	int consume = nk_love_keyevent(key, scancode, isrepeat, 1);
	lua_pushboolean(L, consume);
	return 1;
}

static int nk_love_keyreleased(lua_State *L)
{
	nk_love_assert_argc(lua_gettop(L) == 2);
	const char *key = luaL_checkstring(L, 1);
	const char *scancode = luaL_checkstring(L, 2);
	int consume = nk_love_keyevent(key, scancode, 0, 0);
	lua_pushboolean(L, consume);
	return 1;
}

static int nk_love_mousepressed(lua_State *L)
{
	nk_love_assert_argc(lua_gettop(L) == 4);
	int x = luaL_checkint(L, 1);
	int y = luaL_checkint(L, 2);
	int button = luaL_checkint(L, 3);
	int istouch = nk_love_checkboolean(L, 4);
	int consume = nk_love_clickevent(x, y, button, istouch, 1);
	lua_pushboolean(L, consume);
	return 1;
}

static int nk_love_mousereleased(lua_State *L)
{
	nk_love_assert_argc(lua_gettop(L) == 4);
	int x = luaL_checkint(L, 1);
	int y = luaL_checkint(L, 2);
	int button = luaL_checkint(L, 3);
	int istouch = nk_love_checkboolean(L, 4);
	int consume = nk_love_clickevent(x, y, button, istouch, 0);
	lua_pushboolean(L, consume);
	return 1;
}

static int nk_love_mousemoved(lua_State *L)
{
	nk_love_assert_argc(lua_gettop(L) == 5);
	int x = luaL_checkint(L, 1);
	int y = luaL_checkint(L, 2);
	int dx = luaL_checkint(L, 3);
	int dy = luaL_checkint(L, 4);
	int istouch = nk_love_checkboolean(L, 5);
	int consume = nk_love_mousemoved_event(x, y, dx, dy, istouch);
	lua_pushboolean(L, consume);
	return 1;
}

static int nk_love_textinput(lua_State *L)
{
	nk_love_assert_argc(lua_gettop(L) == 1);
	const char *text = luaL_checkstring(L, 1);
	int consume = nk_love_textinput_event(text);
	lua_pushboolean(L, consume);
	return 1;
}

static int nk_love_wheelmoved(lua_State *L)
{
	nk_love_assert_argc(lua_gettop(L) == 2);
	int x = luaL_checkint(L, 1);
	int y = luaL_checkint(L, 2);
	int consume = nk_love_wheelmoved_event(x, y);
	lua_pushboolean(L, consume);
	return 1;
}

static int nk_love_draw(lua_State *L)
{
	lg->push(love::graphics::Graphics::StackType::STACK_ALL);

	const struct nk_command *cmd;
	nk_foreach(cmd, &context)
	{
		switch (cmd->type) {
		case NK_COMMAND_NOP: break;
		case NK_COMMAND_SCISSOR: {
			const struct nk_command_scissor *s =(const struct nk_command_scissor*)cmd;
			nk_love_scissor(s->x, s->y, s->w, s->h);
		} break;
		case NK_COMMAND_LINE: {
			const struct nk_command_line *l = (const struct nk_command_line *)cmd;
			nk_love_draw_line(l->begin.x, l->begin.y, l->end.x,
				l->end.y, l->line_thickness, l->color);
		} break;
		case NK_COMMAND_RECT: {
			const struct nk_command_rect *r = (const struct nk_command_rect *)cmd;
			nk_love_draw_rect(r->x, r->y, r->w, r->h,
				(unsigned int)r->rounding, r->line_thickness, r->color);
		} break;
		case NK_COMMAND_RECT_FILLED: {
			const struct nk_command_rect_filled *r = (const struct nk_command_rect_filled *)cmd;
			nk_love_draw_rect(r->x, r->y, r->w, r->h, (unsigned int)r->rounding, -1, r->color);
		} break;
		case NK_COMMAND_CIRCLE: {
			const struct nk_command_circle *c = (const struct nk_command_circle *)cmd;
			nk_love_draw_circle(c->x, c->y, c->w, c->h, c->line_thickness, c->color);
		} break;
		case NK_COMMAND_CIRCLE_FILLED: {
			const struct nk_command_circle_filled *c = (const struct nk_command_circle_filled *)cmd;
			nk_love_draw_circle(c->x, c->y, c->w, c->h, -1, c->color);
		} break;
		case NK_COMMAND_TRIANGLE: {
			const struct nk_command_triangle*t = (const struct nk_command_triangle*)cmd;
			nk_love_draw_triangle(t->a.x, t->a.y, t->b.x, t->b.y,
				t->c.x, t->c.y, t->line_thickness, t->color);
		} break;
		case NK_COMMAND_TRIANGLE_FILLED: {
			const struct nk_command_triangle_filled *t = (const struct nk_command_triangle_filled *)cmd;
			nk_love_draw_triangle(t->a.x, t->a.y, t->b.x, t->b.y, t->c.x, t->c.y, -1, t->color);
		} break;
		case NK_COMMAND_POLYGON: {
			const struct nk_command_polygon *p =(const struct nk_command_polygon*)cmd;
			nk_love_draw_polygon(p->points, p->point_count, p->line_thickness, p->color);
		} break;
		case NK_COMMAND_POLYGON_FILLED: {
			const struct nk_command_polygon_filled *p = (const struct nk_command_polygon_filled*)cmd;
			nk_love_draw_polygon(p->points, p->point_count, -1, p->color);
		} break;
		case NK_COMMAND_POLYLINE: {
			const struct nk_command_polyline *p = (const struct nk_command_polyline *)cmd;
			nk_love_draw_polyline(p->points, p->point_count, p->line_thickness, p->color);
		} break;
		case NK_COMMAND_TEXT: {
			const struct nk_command_text *t = (const struct nk_command_text*)cmd;
			nk_love_draw_text(t->font->userdata.id, t->background,
				t->foreground, t->x, t->y, t->w, t->h,
				t->height, t->length, (const char*)t->string);
		} break;
		case NK_COMMAND_CURVE: {
			const struct nk_command_curve *q = (const struct nk_command_curve *)cmd;
			nk_love_draw_curve(q->begin, q->ctrl[0], q->ctrl[1],
				q->end, 22, q->line_thickness, q->color);
		} break;
		case NK_COMMAND_RECT_MULTI_COLOR: {
			const struct nk_command_rect_multi_color *r = (const struct nk_command_rect_multi_color *)cmd;
			nk_love_draw_rect_multi_color(r->x, r->y, r->w, r->h, r->left, r->top, r->right, r->bottom);
		} break;
		case NK_COMMAND_IMAGE: {
			const struct nk_command_image *i = (const struct nk_command_image *)cmd;
			nk_love_draw_image(i->x, i->y, i->w, i->h, i->img, i->col);
		} break;
		case NK_COMMAND_ARC: {
			const struct nk_command_arc *a = (const struct nk_command_arc *)cmd;
			nk_love_draw_arc(a->cx, a->cy, a->r, a->line_thickness,
				a->a[0], a->a[1], a->color);
		} break;
		case NK_COMMAND_ARC_FILLED: {
			const struct nk_command_arc_filled *a = (const struct nk_command_arc_filled *)cmd;
			nk_love_draw_arc(a->cx, a->cy, a->r, -1, a->a[0], a->a[1], a->color);
		} break;
		default: break;
		}
	}

	lg->pop();
	nk_clear(&context);
	return 0;
}

static void nk_love_preserve(struct nk_style_item *item)
{
	if (item->type == NK_STYLE_ITEM_IMAGE) {
		lua_rawgeti(L, -1, item->data.image.handle.id);
		nk_love_checkImage(-1, &item->data.image);
		lua_pop(L, 1);
	}
}

static void nk_love_preserve_all(void)
{
	nk_love_preserve(&context.style.button.normal);
	nk_love_preserve(&context.style.button.hover);
	nk_love_preserve(&context.style.button.active);

	nk_love_preserve(&context.style.contextual_button.normal);
	nk_love_preserve(&context.style.contextual_button.hover);
	nk_love_preserve(&context.style.contextual_button.active);

	nk_love_preserve(&context.style.menu_button.normal);
	nk_love_preserve(&context.style.menu_button.hover);
	nk_love_preserve(&context.style.menu_button.active);

	nk_love_preserve(&context.style.option.normal);
	nk_love_preserve(&context.style.option.hover);
	nk_love_preserve(&context.style.option.active);
	nk_love_preserve(&context.style.option.cursor_normal);
	nk_love_preserve(&context.style.option.cursor_hover);

	nk_love_preserve(&context.style.checkbox.normal);
	nk_love_preserve(&context.style.checkbox.hover);
	nk_love_preserve(&context.style.checkbox.active);
	nk_love_preserve(&context.style.checkbox.cursor_normal);
	nk_love_preserve(&context.style.checkbox.cursor_hover);

	nk_love_preserve(&context.style.selectable.normal);
	nk_love_preserve(&context.style.selectable.hover);
	nk_love_preserve(&context.style.selectable.pressed);
	nk_love_preserve(&context.style.selectable.normal_active);
	nk_love_preserve(&context.style.selectable.hover_active);
	nk_love_preserve(&context.style.selectable.pressed_active);

	nk_love_preserve(&context.style.slider.normal);
	nk_love_preserve(&context.style.slider.hover);
	nk_love_preserve(&context.style.slider.active);
	nk_love_preserve(&context.style.slider.cursor_normal);
	nk_love_preserve(&context.style.slider.cursor_hover);
	nk_love_preserve(&context.style.slider.cursor_active);

	nk_love_preserve(&context.style.progress.normal);
	nk_love_preserve(&context.style.progress.hover);
	nk_love_preserve(&context.style.progress.active);
	nk_love_preserve(&context.style.progress.cursor_normal);
	nk_love_preserve(&context.style.progress.cursor_hover);
	nk_love_preserve(&context.style.progress.cursor_active);

	nk_love_preserve(&context.style.property.normal);
	nk_love_preserve(&context.style.property.hover);
	nk_love_preserve(&context.style.property.active);
	nk_love_preserve(&context.style.property.edit.normal);
	nk_love_preserve(&context.style.property.edit.hover);
	nk_love_preserve(&context.style.property.edit.active);
	nk_love_preserve(&context.style.property.inc_button.normal);
	nk_love_preserve(&context.style.property.inc_button.hover);
	nk_love_preserve(&context.style.property.inc_button.active);
	nk_love_preserve(&context.style.property.dec_button.normal);
	nk_love_preserve(&context.style.property.dec_button.hover);
	nk_love_preserve(&context.style.property.dec_button.active);

	nk_love_preserve(&context.style.edit.normal);
	nk_love_preserve(&context.style.edit.hover);
	nk_love_preserve(&context.style.edit.active);
	nk_love_preserve(&context.style.edit.scrollbar.normal);
	nk_love_preserve(&context.style.edit.scrollbar.hover);
	nk_love_preserve(&context.style.edit.scrollbar.active);
	nk_love_preserve(&context.style.edit.scrollbar.cursor_normal);
	nk_love_preserve(&context.style.edit.scrollbar.cursor_hover);
	nk_love_preserve(&context.style.edit.scrollbar.cursor_active);

	nk_love_preserve(&context.style.chart.background);

	nk_love_preserve(&context.style.scrollh.normal);
	nk_love_preserve(&context.style.scrollh.hover);
	nk_love_preserve(&context.style.scrollh.active);
	nk_love_preserve(&context.style.scrollh.cursor_normal);
	nk_love_preserve(&context.style.scrollh.cursor_hover);
	nk_love_preserve(&context.style.scrollh.cursor_active);

	nk_love_preserve(&context.style.scrollv.normal);
	nk_love_preserve(&context.style.scrollv.hover);
	nk_love_preserve(&context.style.scrollv.active);
	nk_love_preserve(&context.style.scrollv.cursor_normal);
	nk_love_preserve(&context.style.scrollv.cursor_hover);
	nk_love_preserve(&context.style.scrollv.cursor_active);

	nk_love_preserve(&context.style.tab.background);
	nk_love_preserve(&context.style.tab.tab_maximize_button.normal);
	nk_love_preserve(&context.style.tab.tab_maximize_button.hover);
	nk_love_preserve(&context.style.tab.tab_maximize_button.active);
	nk_love_preserve(&context.style.tab.tab_minimize_button.normal);
	nk_love_preserve(&context.style.tab.tab_minimize_button.hover);
	nk_love_preserve(&context.style.tab.tab_minimize_button.active);
	nk_love_preserve(&context.style.tab.node_maximize_button.normal);
	nk_love_preserve(&context.style.tab.node_maximize_button.hover);
	nk_love_preserve(&context.style.tab.node_maximize_button.active);
	nk_love_preserve(&context.style.tab.node_minimize_button.normal);
	nk_love_preserve(&context.style.tab.node_minimize_button.hover);
	nk_love_preserve(&context.style.tab.node_minimize_button.active);

	nk_love_preserve(&context.style.combo.normal);
	nk_love_preserve(&context.style.combo.hover);
	nk_love_preserve(&context.style.combo.active);
	nk_love_preserve(&context.style.combo.button.normal);
	nk_love_preserve(&context.style.combo.button.hover);
	nk_love_preserve(&context.style.combo.button.active);

	nk_love_preserve(&context.style.window.fixed_background);
	nk_love_preserve(&context.style.window.scaler);
	nk_love_preserve(&context.style.window.header.normal);
	nk_love_preserve(&context.style.window.header.hover);
	nk_love_preserve(&context.style.window.header.active);
	nk_love_preserve(&context.style.window.header.close_button.normal);
	nk_love_preserve(&context.style.window.header.close_button.hover);
	nk_love_preserve(&context.style.window.header.close_button.active);
	nk_love_preserve(&context.style.window.header.minimize_button.normal);
	nk_love_preserve(&context.style.window.header.minimize_button.hover);
	nk_love_preserve(&context.style.window.header.minimize_button.active);
}

static int nk_love_frame_begin(lua_State *L)
{
	nk_love_assert_argc(lua_gettop(L) == 0);
	nk_input_end(&context);
	lua_getglobal(L, "love");
	lua_getfield(L, -1, "timer");
	lua_getfield(L, -1, "getDelta");
	lua_call(L, 0, 1);
	float dt = lua_tonumber(L, -1);
	context.delta_time_seconds = dt;
	lua_getfield(L, LUA_REGISTRYINDEX, "nuklear");
	lua_getfield(L, -1, "image");
	lua_newtable(L);
	lua_setfield(L, -3, "image");
	nk_love_preserve_all();
	lua_pop(L, 1);
	lua_getfield(L, -1, "font");
	lua_newtable(L);
	lua_setfield(L, -3, "font");
	font_count = 0;
	lua_rawgeti(L, -1, context.style.font->userdata.id);
	nk_love_checkFont(-1, &fonts[font_count]);
	lua_pop(L, 1);
	context.style.font = &fonts[font_count++];
	int i;
	for (i = 0; i < context.stacks.fonts.head; ++i) {
		struct nk_config_stack_user_font_element *element = &context.stacks.fonts.elements[i];
		lua_rawgeti(L, -1, element->old_value->userdata.id);
		nk_love_checkFont(-1, &fonts[font_count]);
		lua_pop(L, 1);
		context.stacks.fonts.elements[i].old_value = &fonts[font_count++];
	}
	layout_ratio_count = 0;
	return 0;
}

static int nk_love_frame_end(lua_State *L)
{
	nk_love_assert_argc(lua_gettop(L) == 0);
	nk_input_begin(&context);
	return 0;
}

static int nk_love_window_begin(lua_State *L)
{
	const char *name, *title;
	int bounds_begin;
	if (lua_isnumber(L, 2)) {
		nk_love_assert_argc(lua_gettop(L) >= 5);
		name = title = luaL_checkstring(L, 1);
		bounds_begin = 2;
	} else {
		nk_love_assert_argc(lua_gettop(L) >= 6);
		name = luaL_checkstring(L, 1);
		title = luaL_checkstring(L, 2);
		bounds_begin = 3;
	}
	nk_flags flags = nk_love_parse_window_flags(bounds_begin + 4);
	float x = luaL_checknumber(L, bounds_begin);
	float y = luaL_checknumber(L, bounds_begin + 1);
	float width = luaL_checknumber(L, bounds_begin + 2);
	float height = luaL_checknumber(L, bounds_begin + 3);
	int open = nk_begin_titled(&context, name, title, nk_rect(x, y, width, height), flags);
	lua_pushboolean(L, open);
	return 1;
}

static int nk_love_window_end(lua_State *L)
{
	nk_love_assert_argc(lua_gettop(L) == 0);
	nk_end(&context);
	return 0;
}

static int nk_love_window_get_bounds(lua_State *L)
{
	nk_love_assert_argc(lua_gettop(L) == 0);
	struct nk_rect rect = nk_window_get_bounds(&context);
	lua_pushnumber(L, rect.x);
	lua_pushnumber(L, rect.y);
	lua_pushnumber(L, rect.w);
	lua_pushnumber(L, rect.h);
	return 4;
}

static int nk_love_window_get_position(lua_State *L)
{
	nk_love_assert_argc(lua_gettop(L) == 0);
	struct nk_vec2 pos = nk_window_get_position(&context);
	lua_pushnumber(L, pos.x);
	lua_pushnumber(L, pos.y);
	return 2;
}

static int nk_love_window_get_size(lua_State *L)
{
	nk_love_assert_argc(lua_gettop(L) == 0);
	struct nk_vec2 size = nk_window_get_size(&context);
	lua_pushnumber(L, size.x);
	lua_pushnumber(L, size.y);
	return 2;
}

static int nk_love_window_get_content_region(lua_State *L)
{
	nk_love_assert_argc(lua_gettop(L) == 0);
	struct nk_rect rect = nk_window_get_content_region(&context);
	lua_pushnumber(L, rect.x);
	lua_pushnumber(L, rect.y);
	lua_pushnumber(L, rect.w);
	lua_pushnumber(L, rect.h);
	return 4;
}

static int nk_love_window_has_focus(lua_State *L)
{
	nk_love_assert_argc(lua_gettop(L) == 0);
	int has_focus = nk_window_has_focus(&context);
	lua_pushboolean(L, has_focus);
	return 1;
}

static int nk_love_window_is_collapsed(lua_State *L)
{
	nk_love_assert_argc(lua_gettop(L) == 1);
	const char *name = luaL_checkstring(L, 1);
	int is_collapsed = nk_window_is_collapsed(&context, name);
	lua_pushboolean(L, is_collapsed);
	return 1;
}

static int nk_love_window_is_hidden(lua_State *L)
{
	nk_love_assert_argc(lua_gettop(L) == 1);
	const char *name = luaL_checkstring(L, 1);
	int is_hidden = nk_window_is_hidden(&context, name);
	lua_pushboolean(L, is_hidden);
	return 1;
}

static int nk_love_window_is_active(lua_State *L) {
	nk_love_assert_argc(lua_gettop(L) == 1);
	const char *name = luaL_checkstring(L, 1);
	int is_active = nk_window_is_active(&context, name);
	lua_pushboolean(L, is_active);
	return 1;
}

static int nk_love_window_is_hovered(lua_State *L)
{
	nk_love_assert_argc(lua_gettop(L) == 0);
	int is_hovered = nk_window_is_hovered(&context);
	lua_pushboolean(L, is_hovered);
	return 1;
}

static int nk_love_window_is_any_hovered(lua_State *L)
{
	nk_love_assert_argc(lua_gettop(L) == 0);
	int is_any_hovered = nk_window_is_any_hovered(&context);
	lua_pushboolean(L, is_any_hovered);
	return 1;
}

static int nk_love_item_is_any_active(lua_State *L)
{
	nk_love_assert_argc(lua_gettop(L) == 0);
	lua_pushboolean(L, nk_love_is_active(&context));
	return 1;
}

static int nk_love_window_set_bounds(lua_State *L)
{
	nk_love_assert_argc(lua_gettop(L) == 4);
	struct nk_rect bounds;
	bounds.x = luaL_checknumber(L, 1);
	bounds.y = luaL_checknumber(L, 2);
	bounds.w = luaL_checknumber(L, 3);
	bounds.h = luaL_checknumber(L, 4);
	nk_window_set_bounds(&context, bounds);
	return 0;
}

static int nk_love_window_set_position(lua_State *L)
{
	nk_love_assert_argc(lua_gettop(L) == 2);
	struct nk_vec2 pos;
	pos.x = luaL_checknumber(L, 1);
	pos.y = luaL_checknumber(L, 2);
	nk_window_set_position(&context, pos);
	return 0;
}

static int nk_love_window_set_size(lua_State *L)
{
	nk_love_assert_argc(lua_gettop(L) == 2);
	struct nk_vec2 size;
	size.x = luaL_checknumber(L, 1);
	size.y = luaL_checknumber(L, 2);
	nk_window_set_size(&context, size);
	return 0;
}

static int nk_love_window_set_focus(lua_State *L)
{
	nk_love_assert_argc(lua_gettop(L) == 1);
	const char *name = luaL_checkstring(L, 1);
	nk_window_set_focus(&context, name);
	return 0;
}

static int nk_love_window_close(lua_State *L)
{
	nk_love_assert_argc(lua_gettop(L) == 1);
	const char *name = luaL_checkstring(L, 1);
	nk_window_close(&context, name);
	return 0;
}

static int nk_love_window_collapse(lua_State *L)
{
	nk_love_assert_argc(lua_gettop(L) == 1);
	const char *name = luaL_checkstring(L, 1);
	nk_window_collapse(&context, name, NK_MINIMIZED);
	return 0;
}

static int nk_love_window_expand(lua_State *L)
{
	nk_love_assert_argc(lua_gettop(L) == 1);
	const char *name = luaL_checkstring(L, 1);
	nk_window_collapse(&context, name, NK_MAXIMIZED);
	return 0;
}

static int nk_love_window_show(lua_State *L)
{
	nk_love_assert_argc(lua_gettop(L) == 1);
	const char *name = luaL_checkstring(L, 1);
	nk_window_show(&context, name, NK_SHOWN);
	return 0;
}

static int nk_love_window_hide(lua_State *L)
{
	nk_love_assert_argc(lua_gettop(L) == 1);
	const char *name = luaL_checkstring(L, 1);
	nk_window_show(&context, name, NK_HIDDEN);
	return 0;
}

static int nk_love_layout_row(lua_State *L)
{
	int argc = lua_gettop(L);
	nk_love_assert_argc(argc >= 3 && argc <= 4);
	enum nk_layout_format format = nk_love_checkformat(1);
	float height = luaL_checknumber(L, 2);
	int use_ratios = 0;
	if (format == NK_DYNAMIC) {
		nk_love_assert_argc(argc == 3);
		if (lua_isnumber(L, 3)) {
			int cols = luaL_checkint(L, 3);
			nk_layout_row_dynamic(&context, height, cols);
		} else {
			if (!lua_istable(L, 3))
				luaL_argerror(L, 3, "should be a number or table");
			use_ratios = 1;
		}
	} else if (format == NK_STATIC) {
		if (argc == 4) {
			int item_width = luaL_checkint(L, 3);
			int cols = luaL_checkint(L, 4);
			nk_layout_row_static(&context, height, item_width, cols);
		} else {
			if (!lua_istable(L, 3))
				luaL_argerror(L, 3, "should be a number or table");
			use_ratios = 1;
		}
	}
	if (use_ratios) {
		int cols = lua_objlen(L, -1);
		int i, j;
		for (i = 1, j = layout_ratio_count; i <= cols && j < NK_LOVE_MAX_RATIOS; ++i, ++j) {
			lua_rawgeti(L, -1, i);
			if (!lua_isnumber(L, -1))
				luaL_argerror(L, lua_gettop(L) - 1, "should contain numbers only");
			floats[j] = lua_tonumber(L, -1);
			lua_pop(L, 1);
		}
		nk_layout_row(&context, format, height, cols, floats + layout_ratio_count);
		layout_ratio_count += cols;
	}
	return 0;
}

static int nk_love_layout_row_begin(lua_State *L)
{
	nk_love_assert_argc(lua_gettop(L) == 3);
	enum nk_layout_format format = nk_love_checkformat(1);
	float height = luaL_checknumber(L, 2);
	int cols = luaL_checkint(L, 3);
	nk_layout_row_begin(&context, format, height, cols);
	return 0;
}

static int nk_love_layout_row_push(lua_State *L)
{
	nk_love_assert_argc(lua_gettop(L) == 1);
	float value = luaL_checknumber(L, 1);
	nk_layout_row_push(&context, value);
	return 0;
}

static int nk_love_layout_row_end(lua_State *L)
{
	nk_love_assert_argc(lua_gettop(L) == 0);
	nk_layout_row_end(&context);
	return 0;
}

static int nk_love_layout_space_begin(lua_State *L)
{
	nk_love_assert_argc(lua_gettop(L) == 3);
	enum nk_layout_format format = nk_love_checkformat(1);
	float height = luaL_checknumber(L, 2);
	int widget_count = luaL_checkint(L, 3);
	nk_layout_space_begin(&context, format, height, widget_count);
	return 0;
}

static int nk_love_layout_space_push(lua_State *L)
{
	nk_love_assert_argc(lua_gettop(L) == 4);
	float x = luaL_checknumber(L, 1);
	float y = luaL_checknumber(L, 2);
	float width = luaL_checknumber(L, 3);
	float height = luaL_checknumber(L, 4);
	nk_layout_space_push(&context, nk_rect(x, y, width, height));
	return 0;
}

static int nk_love_layout_space_end(lua_State *L)
{
	nk_love_assert_argc(lua_gettop(L) == 0);
	nk_layout_space_end(&context);
	return 0;
}

static int nk_love_layout_space_bounds(lua_State *L)
{
	nk_love_assert_argc(lua_gettop(L) == 0);
	struct nk_rect bounds = nk_layout_space_bounds(&context);
	lua_pushnumber(L, bounds.x);
	lua_pushnumber(L, bounds.y);
	lua_pushnumber(L, bounds.w);
	lua_pushnumber(L, bounds.h);
	return 4;
}

static int nk_love_layout_space_to_screen(lua_State *L)
{
	nk_love_assert_argc(lua_gettop(L) == 2);
	struct nk_vec2 local;
	local.x = luaL_checknumber(L, 1);
	local.y = luaL_checknumber(L, 2);
	struct nk_vec2 screen = nk_layout_space_to_screen(&context, local);
	lua_pushnumber(L, screen.x);
	lua_pushnumber(L, screen.y);
	return 2;
}

static int nk_love_layout_space_to_local(lua_State *L)
{
	nk_love_assert_argc(lua_gettop(L) == 2);
	struct nk_vec2 screen;
	screen.x = luaL_checknumber(L, 1);
	screen.y = luaL_checknumber(L, 2);
	struct nk_vec2 local = nk_layout_space_to_local(&context, screen);
	lua_pushnumber(L, local.x);
	lua_pushnumber(L, local.y);
	return 2;
}

static int nk_love_layout_space_rect_to_screen(lua_State *L)
{
	nk_love_assert_argc(lua_gettop(L) == 4);
	struct nk_rect local;
	local.x = luaL_checknumber(L, 1);
	local.y = luaL_checknumber(L, 2);
	local.w = luaL_checknumber(L, 3);
	local.h = luaL_checknumber(L, 4);
	struct nk_rect screen = nk_layout_space_rect_to_screen(&context, local);
	lua_pushnumber(L, screen.x);
	lua_pushnumber(L, screen.y);
	lua_pushnumber(L, screen.w);
	lua_pushnumber(L, screen.h);
	return 4;
}

static int nk_love_layout_space_rect_to_local(lua_State *L)
{
	nk_love_assert_argc(lua_gettop(L) == 4);
	struct nk_rect screen;
	screen.x = luaL_checknumber(L, 1);
	screen.y = luaL_checknumber(L, 2);
	screen.w = luaL_checknumber(L, 3);
	screen.h = luaL_checknumber(L, 4);
	struct nk_rect local = nk_layout_space_rect_to_screen(&context, screen);
	lua_pushnumber(L, local.x);
	lua_pushnumber(L, local.y);
	lua_pushnumber(L, local.w);
	lua_pushnumber(L, local.h);
	return 4;
}

static int nk_love_layout_ratio_from_pixel(lua_State *L)
{
	nk_love_assert_argc(lua_gettop(L) == 1);
	float pixel_width = luaL_checknumber(L, 1);
	float ratio = nk_layout_ratio_from_pixel(&context, pixel_width);
	lua_pushnumber(L, ratio);
	return 1;
}

static int nk_love_group_begin(lua_State *L)
{
	nk_love_assert_argc(lua_gettop(L) >= 1);
	const char *title = luaL_checkstring(L, 1);
	nk_flags flags = nk_love_parse_window_flags(2);
	int open = nk_group_begin(&context, title, flags);
	lua_pushboolean(L, open);
	return 1;
}

static int nk_love_group_end(lua_State *L)
{
	nk_love_assert_argc(lua_gettop(L) == 0);
	nk_group_end(&context);
	return 0;
}

static int nk_love_tree_push(lua_State *L)
{
	int argc = lua_gettop(L);
	nk_love_assert_argc(argc >= 2 && argc <= 4);
	enum nk_tree_type type = nk_love_checktree(1);
	const char *title = luaL_checkstring(L, 2);
	struct nk_image image;
	int use_image = 0;
	if (argc >= 3 && !lua_isnil(L, 3)) {
		nk_love_checkImage(3, &image);
		use_image = 1;
	}
	enum nk_collapse_states state = NK_MINIMIZED;
	if (argc >= 4)
		state = nk_love_checkstate(4);
	lua_Debug ar;
	lua_getstack(L, 1, &ar);
	lua_getinfo(L, "l", &ar);
	int id = ar.currentline;
	int open = 0;
	if (use_image)
		open = nk_tree_image_push_hashed(&context, type, image, title, state, title, strlen(title), id);
	else
		open = nk_tree_push_hashed(&context, type, title, state, title, strlen(title), id);
	lua_pushboolean(L, open);
	return 1;
}

static int nk_love_tree_pop(lua_State *L)
{
	nk_love_assert_argc(lua_gettop(L) == 0);
	nk_tree_pop(&context);
	return 0;
}

static void nk_love_color(int r, int g, int b, int a, char *color_string)
{
	r = NK_CLAMP(0, r, 255);
	g = NK_CLAMP(0, g, 255);
	b = NK_CLAMP(0, b, 255);
	a = NK_CLAMP(0, a, 255);
	const char *format_string;
	if (a < 255) {
		format_string = "#%02x%02x%02x%02x";
	} else {
		format_string = "#%02x%02x%02x";
	}
	sprintf(color_string, format_string, r, g, b, a);
}

static int nk_love_color_rgba(lua_State *L)
{
	int argc = lua_gettop(L);
	nk_love_assert_argc(argc == 3 || argc == 4);
	int r = luaL_checkint(L, 1);
	int g = luaL_checkint(L, 2);
	int b = luaL_checkint(L, 3);
	int a = 255;
	if (argc == 4)
		a = luaL_checkint(L, 4);
	char color_string[10];
	nk_love_color(r, g, b, a, color_string);
	lua_pushstring(L, color_string);
	return 1;
}

static int nk_love_color_hsva(lua_State *L)
{
	int argc = lua_gettop(L);
	nk_love_assert_argc(argc == 3 || argc == 4);
	int h = NK_CLAMP(0, luaL_checkint(L, 1), 255);
	int s = NK_CLAMP(0, luaL_checkint(L, 2), 255);
	int v = NK_CLAMP(0, luaL_checkint(L, 3), 255);
	int a = 255;
	if (argc == 4)
		a = NK_CLAMP(0, luaL_checkint(L, 4), 255);
	struct nk_color rgba = nk_hsva(h, s, v, a);
	char color_string[10];
	nk_love_color(rgba.r, rgba.g, rgba.b, rgba.a, color_string);
	lua_pushstring(L, color_string);
	return 1;
}

static int nk_love_color_parse_rgba(lua_State *L)
{
	nk_love_assert_argc(lua_gettop(L) == 1);
	struct nk_color rgba = nk_love_checkcolor(1);
	lua_pushnumber(L, rgba.r);
	lua_pushnumber(L, rgba.g);
	lua_pushnumber(L, rgba.b);
	lua_pushnumber(L, rgba.a);
	return 4;
}

static int nk_love_color_parse_hsva(lua_State *L)
{
	nk_love_assert_argc(lua_gettop(L) == 1);
	struct nk_color rgba = nk_love_checkcolor(1);
	int h, s, v, a2;
	nk_color_hsva_i(&h, &s, &v, &a2, rgba);
	lua_pushnumber(L, h);
	lua_pushnumber(L, s);
	lua_pushnumber(L, v);
	lua_pushnumber(L, a2);
	return 4;
}

static int nk_love_label(lua_State *L)
{
	int argc = lua_gettop(L);
	nk_love_assert_argc(argc >= 1 && argc <= 3);
	const char *text = luaL_checkstring(L, 1);
	nk_flags align = NK_TEXT_LEFT;
	int wrap = 0;
	struct nk_color color;
	int use_color = 0;
	if (argc >= 2) {
		const char *align_string = luaL_checkstring(L, 2);
		if (!strcmp(align_string, "wrap"))
			wrap = 1;
		else
			align = nk_love_checkalign(2);
		if (argc >= 3) {
			color = nk_love_checkcolor(3);
			use_color = 1;
		}
	}
	if (use_color) {
		if (wrap)
			nk_label_colored_wrap(&context, text, color);
		else
			nk_label_colored(&context, text, align, color);
	} else {
		if (wrap)
			nk_label_wrap(&context, text);
		else
			nk_label(&context, text, align);
	}
	return 0;
}

static int nk_love_image(lua_State *L)
{
	int argc = lua_gettop(L);
	nk_love_assert_argc(argc == 1 || argc == 5);
	struct nk_image image;
	nk_love_checkImage(1, &image);
	if (argc == 1) {
		nk_image(&context, image);
	} else {
		float x = luaL_checknumber(L, 2);
		float y = luaL_checknumber(L, 3);
		float w = luaL_checknumber(L, 4);
		float h = luaL_checknumber(L, 5);
		float line_thickness;
		struct nk_color color;
		nk_love_getGraphics(&line_thickness, &color);
		nk_draw_image(&context.current->buffer, nk_rect(x, y, w, h), &image, color);
	}
	return 0;
}

static int nk_love_button(lua_State *L)
{
	int argc = lua_gettop(L);
	nk_love_assert_argc(argc >= 1 && argc <= 2);
	const char *title = NULL;
	if (!lua_isnil(L, 1))
		title = luaL_checkstring(L, 1);
	int use_color = 0, use_image = 0;
	struct nk_color color;
	enum nk_symbol_type symbol = NK_SYMBOL_NONE;
	struct nk_image image;
	if (argc >= 2 && !lua_isnil(L, 2)) {
		if (lua_isstring(L, 2)) {
			if (nk_love_is_color(2)) {
				color = nk_love_checkcolor(2);
				use_color = 1;
			} else {
				symbol = nk_love_checksymbol(2);
			}
		} else {
			nk_love_checkImage(2, &image);
			use_image = 1;
		}
	}
	nk_flags align = context.style.button.text_alignment;
	int activated = 0;
	if (title != NULL) {
		if (use_color)
			nk_love_assert(0, "%s: color buttons can't have titles");
		else if (symbol != NK_SYMBOL_NONE)
			activated = nk_button_symbol_label(&context, symbol, title, align);
		else if (use_image)
			activated = nk_button_image_label(&context, image, title, align);
		else
			activated = nk_button_label(&context, title);
	} else {
		if (use_color)
			activated = nk_button_color(&context, color);
		else if (symbol != NK_SYMBOL_NONE)
			activated = nk_button_symbol(&context, symbol);
		else if (use_image)
			activated = nk_button_image(&context, image);
		else
			nk_love_assert(0, "%s: must specify a title, color, symbol, and/or image");
	}
	lua_pushboolean(L, activated);
	return 1;
}

static int nk_love_button_set_behavior(lua_State *L)
{
	nk_love_assert_argc(lua_gettop(L) == 1);
	enum nk_button_behavior behavior = nk_love_checkbehavior(1);
	nk_button_set_behavior(&context, behavior);
	return 0;
}

static int nk_love_button_push_behavior(lua_State *L)
{
	nk_love_assert_argc(lua_gettop(L) == 1);
	enum nk_button_behavior behavior = nk_love_checkbehavior(1);
	nk_button_push_behavior(&context, behavior);
	return 0;
}

static int nk_love_button_pop_behavior(lua_State *L)
{
	nk_love_assert_argc(lua_gettop(L) == 0);
	nk_button_pop_behavior(&context);
	return 0;
}

static int nk_love_checkbox(lua_State *L)
{
	nk_love_assert_argc(lua_gettop(L) == 2);
	const char *text = luaL_checkstring(L, 1);
	if (lua_isboolean(L, 2)) {
		int value = lua_toboolean(L, 2);
		value = nk_check_label(&context, text, value);
		lua_pushboolean(L, value);
	} else if (lua_istable(L, 2)) {
		lua_getfield(L, 2, "value");
		int value = lua_toboolean(L, -1);
		int changed = nk_checkbox_label(&context, text, &value);
		if (changed) {
			lua_pushboolean(L, value);
			lua_setfield(L, 2, "value");
		}
		lua_pushboolean(L, changed);
	} else {
		luaL_typerror(L, 2, "boolean or table");
	}
	return 1;
}

static int nk_love_radio(lua_State *L)
{
	int argc = lua_gettop(L);
	nk_love_assert_argc(argc == 2 || argc == 3);
	const char *name = luaL_checkstring(L, 1);
	const char *text;
	if (argc == 3)
		text = luaL_checkstring(L, 2);
	else
		text = luaL_checkstring(L, 1);
	if (lua_isstring(L, -1)) {
		const char *value = lua_tostring(L, -1);
		int active = !strcmp(value, name);
		active = nk_option_label(&context, text, active);
		if (active)
			lua_pushstring(L, name);
		else
			lua_pushstring(L, value);
	} else if (lua_istable(L, -1)) {
		lua_getfield(L, -1, "value");
		if (!lua_isstring(L, -1))
			luaL_argerror(L, argc, "should have a string value");
		const char *value = lua_tostring(L, -1);
		int active = !strcmp(value, name);
		int changed = nk_radio_label(&context, text, &active);
		if (changed && active) {
			lua_pushstring(L, name);
			lua_setfield(L, -3, "value");
		}
		lua_pushboolean(L, changed);
	} else {
		luaL_typerror(L, argc, "string or table");
	}
	return 1;
}

static int nk_love_selectable(lua_State *L)
{
	int argc = lua_gettop(L);
	nk_love_assert_argc(argc >= 2 && argc <= 4);
	const char *text = luaL_checkstring(L, 1);
	struct nk_image image;
	int use_image = 0;
	if (argc >= 3 && !lua_isnil(L, 2)) {
		nk_love_checkImage(2, &image);
		use_image = 1;
	}
	nk_flags align = NK_TEXT_LEFT;
	if (argc >= 4)
		align = nk_love_checkalign(3);
	if (lua_isboolean(L, -1)) {
		int value = lua_toboolean(L, -1);
		if (use_image)
			value = nk_select_image_label(&context, image, text, align, value);
		else
			value = nk_select_label(&context, text, align, value);
		lua_pushboolean(L, value);
	} else if (lua_istable(L, -1)) {
		lua_getfield(L, -1, "value");
		if (!lua_isboolean(L, -1))
			luaL_argerror(L, argc, "should have a boolean value");
		int value = lua_toboolean(L, -1);
		int changed;
		if (use_image)
			changed = nk_selectable_image_label(&context, image, text, align, &value);
		else
			changed = nk_selectable_label(&context, text, align, &value);
		if (changed) {
			lua_pushboolean(L, value);
			lua_setfield(L, -3, "value");
		}
		lua_pushboolean(L, changed);
	} else {
		luaL_typerror(L, argc, "boolean or table");
	}
	return 1;
}

static int nk_love_slider(lua_State *L)
{
	nk_love_assert_argc(lua_gettop(L) == 4);
	float min = luaL_checknumber(L, 1);
	float max = luaL_checknumber(L, 3);
	float step = luaL_checknumber(L, 4);
	if (lua_isnumber(L, 2)) {
		float value = lua_tonumber(L, 2);
		value = nk_slide_float(&context, min, value, max, step);
		lua_pushnumber(L, value);
	} else if (lua_istable(L, 2)) {
		lua_getfield(L, 2, "value");
		if (!lua_isnumber(L, -1))
			luaL_argerror(L, 2, "should have a number value");
		float value = lua_tonumber(L, -1);
		int changed = nk_slider_float(&context, min, &value, max, step);
		if (changed) {
			lua_pushnumber(L, value);
			lua_setfield(L, 2, "value");
		}
		lua_pushboolean(L, changed);
	} else {
		luaL_typerror(L, 2, "number or table");
	}
	return 1;
}

static int nk_love_progress(lua_State *L)
{
	int argc = lua_gettop(L);
	nk_love_assert_argc(argc >= 2 || argc <= 3);
	nk_size max = luaL_checklong(L, 2);
	int modifiable = 0;
	if (argc >= 3 && !lua_isnil(L, 3))
		modifiable = nk_love_checkboolean(L, 3);
	if (lua_isnumber(L, 1)) {
		nk_size value = lua_tonumber(L, 1);
		value = nk_prog(&context, value, max, modifiable);
		lua_pushnumber(L, value);
	} else if (lua_istable(L, 1)) {
		lua_getfield(L, 1, "value");
		if (!lua_isnumber(L, -1))
			luaL_argerror(L, 1, "should have a number value");
		nk_size value = (nk_size) lua_tonumber(L, -1);
		int changed = nk_progress(&context, &value, max, modifiable);
		if (changed) {
			lua_pushnumber(L, value);
			lua_setfield(L, 1, "value");
		}
		lua_pushboolean(L, changed);
	} else {
		luaL_typerror(L, 1, "number or table");
	}
	return 1;
}

static int nk_love_color_picker(lua_State *L)
{
	int argc = lua_gettop(L);
	nk_love_assert_argc(argc >= 1 && argc <= 2);
	enum nk_color_format format = NK_RGB;
	if (argc >= 2)
		format = nk_love_checkcolorformat(2);
	if (lua_isstring(L, 1)) {
		struct nk_color color = nk_love_checkcolor(1);
		color = nk_color_picker(&context, color, format);
		char new_color_string[10];
		nk_love_color(color.r, color.g, color.b, color.a, new_color_string);
		lua_pushstring(L, new_color_string);
	} else if (lua_istable(L, 1)) {
		lua_getfield(L, 1, "value");
		if (!nk_love_is_color(-1))
			luaL_argerror(L, 1, "should have a color string value");
		struct nk_color color = nk_love_checkcolor(-1);
		int changed = nk_color_pick(&context, &color, format);
		if (changed) {
			char new_color_string[10];
			nk_love_color(color.r, color.g, color.b, color.a, new_color_string);
			lua_pushstring(L, new_color_string);
			lua_setfield(L, 1, "value");
		}
		lua_pushboolean(L, changed);
	} else {
		luaL_typerror(L, 1, "string or table");
	}
	return 1;
}

static int nk_love_property(lua_State *L)
{
	nk_love_assert_argc(lua_gettop(L) == 6);
	const char *name = luaL_checkstring(L, 1);
	double min = luaL_checknumber(L, 2);
	double max = luaL_checknumber(L, 4);
	double step = luaL_checknumber(L, 5);
	float inc_per_pixel = luaL_checknumber(L, 6);
	if (lua_isnumber(L, 3)) {
		double value = lua_tonumber(L, 3);
		value = nk_propertyd(&context, name, min, value, max, step, inc_per_pixel);
		lua_pushnumber(L, value);
	} else if (lua_istable(L, 3)) {
		lua_getfield(L, 3, "value");
		if (!lua_isnumber(L, -1))
			luaL_argerror(L, 3, "should have a number value");
		double value = lua_tonumber(L, -1);
		double old = value;
		nk_property_double(&context, name, min, &value, max, step, inc_per_pixel);
		int changed = value != old;
		if (changed) {
			lua_pushnumber(L, value);
			lua_setfield(L, 3, "value");
		}
		lua_pushboolean(L, changed);
	} else {
		luaL_typerror(L, 3, "number or table");
	}
	return 1;
}

static int nk_love_edit(lua_State *L)
{
	nk_love_assert_argc(lua_gettop(L) == 2);
	nk_flags flags = nk_love_checkedittype(1);
	if (!lua_istable(L, 2))
		luaL_typerror(L, 2, "table");
	lua_getfield(L, 2, "value");
	if (!lua_isstring(L, -1))
		luaL_argerror(L, 2, "should have a string value");
	const char *value = lua_tostring(L, -1);
	size_t len = NK_CLAMP(0, strlen(value), NK_LOVE_EDIT_BUFFER_LEN - 1);
	memcpy(edit_buffer, value, len);
	edit_buffer[len] = '\0';
	nk_flags event = nk_edit_string_zero_terminated(&context, flags, edit_buffer, NK_LOVE_EDIT_BUFFER_LEN - 1, nk_filter_default);
	lua_pushstring(L, edit_buffer);
	lua_pushvalue(L, -1);
	lua_setfield(L, 2, "value");
	int changed = !lua_equal(L, -1, -2);
	if (event & NK_EDIT_COMMITED)
		lua_pushstring(L, "commited");
	else if (event & NK_EDIT_ACTIVATED)
		lua_pushstring(L, "activated");
	else if (event & NK_EDIT_DEACTIVATED)
		lua_pushstring(L, "deactivated");
	else if (event & NK_EDIT_ACTIVE)
		lua_pushstring(L, "active");
	else if (event & NK_EDIT_INACTIVE)
		lua_pushstring(L, "inactive");
	else
		lua_pushnil(L);
	lua_pushboolean(L, changed);
	return 2;
}

static int nk_love_popup_begin(lua_State *L)
{
	nk_love_assert_argc(lua_gettop(L) >= 6);
	enum nk_popup_type type = nk_love_checkpopup(1);
	const char *title = luaL_checkstring(L, 2);
	struct nk_rect bounds;
	bounds.x = luaL_checknumber(L, 3);
	bounds.y = luaL_checknumber(L, 4);
	bounds.w = luaL_checknumber(L, 5);
	bounds.h = luaL_checknumber(L, 6);
	nk_flags flags = nk_love_parse_window_flags(7);
	int open = nk_popup_begin(&context, type, title, flags, bounds);
	lua_pushboolean(L, open);
	return 1;
}

static int nk_love_popup_close(lua_State *L)
{
	nk_love_assert_argc(lua_gettop(L) == 0);
	nk_popup_close(&context);
	return 0;
}

static int nk_love_popup_end(lua_State *L)
{
	nk_love_assert_argc(lua_gettop(L) == 0);
	nk_popup_end(&context);
	return 0;
}

static int nk_love_combobox(lua_State *L)
{
	int argc = lua_gettop(L);
	nk_love_assert_argc(argc >= 2 && argc <= 5);
	if (!lua_istable(L, 2))
		luaL_typerror(L, 2, "table");
	int i;
	for (i = 0; i < NK_LOVE_COMBOBOX_MAX_ITEMS && lua_checkstack(L, 4); ++i) {
		lua_rawgeti(L, 2, i + 1);
		if (lua_isstring(L, -1))
			combobox_items[i] = lua_tostring(L, -1);
		else if (lua_isnil(L, -1))
			break;
		else
			luaL_argerror(L, 2, "items must be strings");
	}
	struct nk_rect bounds = nk_widget_bounds(&context);
	int item_height = bounds.h;
	if (argc >= 3 && !lua_isnil(L, 3))
		item_height = luaL_checkint(L, 3);
	struct nk_vec2 size = nk_vec2(bounds.w, item_height * 8);
	if (argc >= 4 && !lua_isnil(L, 4))
		size.x = luaL_checknumber(L, 4);
	if (argc >= 5 && !lua_isnil(L, 5))
		size.y = luaL_checknumber(L, 5);
	if (lua_isnumber(L, 1)) {
		int value = lua_tointeger(L, 1) - 1;
		value = nk_combo(&context, combobox_items, i, value, item_height, size);
		lua_pushnumber(L, value + 1);
	} else if (lua_istable(L, 1)) {
		lua_getfield(L, 1, "value");
		if (!lua_isnumber(L, -1))
			luaL_argerror(L, 1, "should have a number value");
		int value = lua_tointeger(L, -1) - 1;
		int old = value;
		nk_combobox(&context, combobox_items, i, &value, item_height, size);
		int changed = value != old;
		if (changed) {
			lua_pushnumber(L, value + 1);
			lua_setfield(L, 1, "value");
		}
		lua_pushboolean(L, changed);
	} else {
		luaL_typerror(L, 1, "number or table");
	}
	return 1;
}

static int nk_love_combobox_begin(lua_State *L)
{
	int argc = lua_gettop(L);
	nk_love_assert_argc(argc >= 1 && argc <= 4);
	const char *text = NULL;
	if (!lua_isnil(L, 1))
		text = luaL_checkstring(L, 1);
	struct nk_color color;
	int use_color = 0;
	enum nk_symbol_type symbol = NK_SYMBOL_NONE;
	struct nk_image image;
	int use_image = 0;
	if (argc >= 2 && !lua_isnil(L, 2)) {
		if (lua_isstring(L, 2)) {
			if (nk_love_is_color(2)) {
				color = nk_love_checkcolor(2);
				use_color = 1;
			} else {
				symbol = nk_love_checksymbol(2);
			}
		} else {
			nk_love_checkImage(2, &image);
			use_image = 1;
		}
	}
	struct nk_rect bounds = nk_widget_bounds(&context);
	struct nk_vec2 size = nk_vec2(bounds.w, bounds.h * 8);
	if (argc >= 3 && !lua_isnil(L, 3))
		size.x = luaL_checknumber(L, 3);
	if (argc >= 4 && !lua_isnil(L, 4))
		size.y = luaL_checknumber(L, 4);
	int open = 0;
	if (text != NULL) {
		if (use_color)
			nk_love_assert(0, "%s: color comboboxes can't have titles");
		else if (symbol != NK_SYMBOL_NONE)
			open = nk_combo_begin_symbol_label(&context, text, symbol, size);
		else if (use_image)
			open = nk_combo_begin_image_label(&context, text, image, size);
		else
			open = nk_combo_begin_label(&context, text, size);
	} else {
		if (use_color)
			open = nk_combo_begin_color(&context, color, size);
		else if (symbol != NK_SYMBOL_NONE)
			open = nk_combo_begin_symbol(&context, symbol, size);
		else if (use_image)
			open = nk_combo_begin_image(&context, image, size);
		else
			nk_love_assert(0, "%s: must specify color, symbol, image, and/or title");
	}
	lua_pushboolean(L, open);
	return 1;
}

static int nk_love_combobox_item(lua_State *L)
{
	int argc = lua_gettop(L);
	nk_love_assert_argc(argc >= 1 && argc <= 3);
	const char *text = luaL_checkstring(L, 1);
	enum nk_symbol_type symbol = NK_SYMBOL_NONE;
	struct nk_image image;
	int use_image = 0;
	if (argc >= 2 && !lua_isnil(L, 2)) {
		if (lua_isstring(L, 2)) {
			symbol = nk_love_checksymbol(2);
		} else {
			nk_love_checkImage(2, &image);
			use_image = 1;
		}
	}
	nk_flags align = NK_TEXT_LEFT;
	if (argc >= 3 && !lua_isnil(L, 3))
		align = nk_love_checkalign(3);
	int activated = 0;
	if (symbol != NK_SYMBOL_NONE)
		activated = nk_combo_item_symbol_label(&context, symbol, text, align);
	else if (use_image)
		activated = nk_combo_item_image_label(&context, image, text, align);
	else
		activated = nk_combo_item_label(&context, text, align);
	lua_pushboolean(L, activated);
	return 1;
}

static int nk_love_combobox_close(lua_State *L)
{
	nk_love_assert_argc(lua_gettop(L) == 0);
	nk_combo_close(&context);
	return 0;
}

static int nk_love_combobox_end(lua_State *L)
{
	nk_love_assert_argc(lua_gettop(L) == 0);
	nk_combo_end(&context);
	return 0;
}

static int nk_love_contextual_begin(lua_State *L)
{
	nk_love_assert_argc(lua_gettop(L) >= 6);
	struct nk_vec2 size;
	size.x = luaL_checknumber(L, 1);
	size.y = luaL_checknumber(L, 2);
	struct nk_rect trigger;
	trigger.x = luaL_checknumber(L, 3);
	trigger.y = luaL_checknumber(L, 4);
	trigger.w = luaL_checknumber(L, 5);
	trigger.h = luaL_checknumber(L, 6);
	nk_flags flags = nk_love_parse_window_flags(7);
	int open = nk_contextual_begin(&context, flags, size, trigger);
	lua_pushboolean(L, open);
	return 1;
}

static int nk_love_contextual_item(lua_State *L)
{
	int argc = lua_gettop(L);
	nk_love_assert_argc(argc >= 1 && argc <= 3);
	const char *text = luaL_checkstring(L, 1);
	enum nk_symbol_type symbol = NK_SYMBOL_NONE;
	struct nk_image image;
	int use_image = 0;
	if (argc >= 2 && !lua_isnil(L, 2)) {
		if (lua_isstring(L, 2)) {
			symbol = nk_love_checksymbol(2);
		} else {
			nk_love_checkImage(2, &image);
			use_image = 1;
		}
	}
	nk_flags align = NK_TEXT_LEFT;
	if (argc >= 3 && !lua_isnil(L, 3))
		align = nk_love_checkalign(3);
	int activated;
	if (symbol != NK_SYMBOL_NONE)
		activated = nk_contextual_item_symbol_label(&context, symbol, text, align);
	else if (use_image)
		activated = nk_contextual_item_image_label(&context, image, text, align);
	else
		activated = nk_contextual_item_label(&context, text, align);
	lua_pushboolean(L, activated);
	return 1;
}

static int nk_love_contextual_close(lua_State *L)
{
	nk_love_assert_argc(lua_gettop(L) == 0);
	nk_contextual_close(&context);
	return 0;
}

static int nk_love_contextual_end(lua_State *L)
{
	nk_love_assert_argc(lua_gettop(L) == 0);
	nk_contextual_end(&context);
	return 0;
}

static int nk_love_tooltip(lua_State *L)
{
	nk_love_assert_argc(lua_gettop(L) == 1);
	const char *text = luaL_checkstring(L, 1);
	nk_tooltip(&context, text);
	return 0;
}

static int nk_love_tooltip_begin(lua_State *L)
{
	nk_love_assert_argc(lua_gettop(L) == 1);
	float width = luaL_checknumber(L, 1);
	int open = nk_tooltip_begin(&context, width);
	lua_pushnumber(L, open);
	return 1;
}

static int nk_love_tooltip_end(lua_State *L)
{
	nk_love_assert_argc(lua_gettop(L) == 0);
	nk_tooltip_end(&context);
	return 0;
}

static int nk_love_menubar_begin(lua_State *L)
{
	nk_love_assert_argc(lua_gettop(L) == 0);
	nk_menubar_begin(&context);
	return 0;
}

static int nk_love_menubar_end(lua_State *L)
{
	nk_love_assert_argc(lua_gettop(L) == 0);
	nk_menubar_end(&context);
	return 0;
}

static int nk_love_menu_begin(lua_State *L)
{
	int argc = lua_gettop(L);
	nk_love_assert_argc(argc >= 4 && argc <= 5);
	const char *text = luaL_checkstring(L, 1);
	enum nk_symbol_type symbol = NK_SYMBOL_NONE;
	struct nk_image image;
	int use_image = 0;
	if (lua_isstring(L, 2)) {
		symbol = nk_love_checksymbol(2);
	} else if (!lua_isnil(L, 2)) {
		nk_love_checkImage(2, &image);
		use_image = 1;
	}
	struct nk_vec2 size;
	size.x = luaL_checknumber(L, 3);
	size.y = luaL_checknumber(L, 4);
	nk_flags align = NK_TEXT_LEFT;
	if (argc >= 5 && !lua_isnil(L, 5))
		align = nk_love_checkalign(5);
	int open;
	if (symbol != NK_SYMBOL_NONE)
		open = nk_menu_begin_symbol_label(&context, text, align, symbol, size);
	else if (use_image)
		open = nk_menu_begin_image_label(&context, text, align, image, size);
	else
		open = nk_menu_begin_label(&context, text, align, size);
	lua_pushboolean(L, open);
	return 1;
}

static int nk_love_menu_item(lua_State *L)
{
	int argc = lua_gettop(L);
	nk_love_assert_argc(argc >= 1 && argc <= 3);
	const char *text = luaL_checkstring(L, 1);
	enum nk_symbol_type symbol = NK_SYMBOL_NONE;
	struct nk_image image;
	int use_image = 0;
	if (argc >= 2 && !lua_isnil(L, 2)) {
		if (lua_isstring(L, 2)) {
			symbol = nk_love_checksymbol(2);
		} else {
			nk_love_checkImage(2, &image);
			use_image = 1;
		}
	}
	nk_flags align = NK_TEXT_LEFT;
	if (argc >= 3 && !lua_isnil(L, 3))
		align = nk_love_checkalign(3);
	int activated;
	if (symbol != NK_SYMBOL_NONE)
		activated = nk_menu_item_symbol_label(&context, symbol, text, align);
	else if (use_image)
		activated = nk_menu_item_image_label(&context, image, text, align);
	else
		activated = nk_menu_item_label(&context, text, align);
	lua_pushboolean(L, activated);
	return 1;
}

static int nk_love_menu_close(lua_State *L)
{
	nk_love_assert_argc(lua_gettop(L) == 0);
	nk_menu_close(&context);
	return 0;
}

static int nk_love_menu_end(lua_State *L)
{
	nk_love_assert_argc(lua_gettop(L) == 0);
	nk_menu_end(&context);
	return 0;
}

static int nk_love_style_default(lua_State *L)
{
	nk_love_assert_argc(lua_gettop(L) == 0);
	nk_style_default(&context);
	return 0;
}

#define NK_LOVE_LOAD_COLOR(type) \
	lua_getfield(L, -1, (type)); \
	nk_love_assert(nk_love_is_color(-1), "%s: table missing color value for '" type "'"); \
	colors[index++] = nk_love_checkcolor(-1); \
	lua_pop(L, 1);

static int nk_love_style_load_colors(lua_State *L)
{
	nk_love_assert_argc(lua_gettop(L) == 1);
	if (!lua_istable(L, 1))
		luaL_typerror(L, 1, "table");
	struct nk_color colors[NK_COLOR_COUNT];
	int index = 0;
	NK_LOVE_LOAD_COLOR("text");
	NK_LOVE_LOAD_COLOR("window");
	NK_LOVE_LOAD_COLOR("header");
	NK_LOVE_LOAD_COLOR("border");
	NK_LOVE_LOAD_COLOR("button");
	NK_LOVE_LOAD_COLOR("button hover");
	NK_LOVE_LOAD_COLOR("button active");
	NK_LOVE_LOAD_COLOR("toggle");
	NK_LOVE_LOAD_COLOR("toggle hover");
	NK_LOVE_LOAD_COLOR("toggle cursor");
	NK_LOVE_LOAD_COLOR("select");
	NK_LOVE_LOAD_COLOR("select active");
	NK_LOVE_LOAD_COLOR("slider");
	NK_LOVE_LOAD_COLOR("slider cursor");
	NK_LOVE_LOAD_COLOR("slider cursor hover");
	NK_LOVE_LOAD_COLOR("slider cursor active");
	NK_LOVE_LOAD_COLOR("property");
	NK_LOVE_LOAD_COLOR("edit");
	NK_LOVE_LOAD_COLOR("edit cursor");
	NK_LOVE_LOAD_COLOR("combo");
	NK_LOVE_LOAD_COLOR("chart");
	NK_LOVE_LOAD_COLOR("chart color");
	NK_LOVE_LOAD_COLOR("chart color highlight");
	NK_LOVE_LOAD_COLOR("scrollbar");
	NK_LOVE_LOAD_COLOR("scrollbar cursor");
	NK_LOVE_LOAD_COLOR("scrollbar cursor hover");
	NK_LOVE_LOAD_COLOR("scrollbar cursor active");
	NK_LOVE_LOAD_COLOR("tab header");
	nk_style_from_table(&context, colors);
	return 0;
}

static int nk_love_style_set_font(lua_State *L)
{
	nk_love_assert_argc(lua_gettop(L) == 1);
	nk_love_checkFont(1, &fonts[font_count]);
	nk_style_set_font(&context, &fonts[font_count++]);
	return 0;
}

static int nk_love_style_push_color(struct nk_color *field)
{
	if (!nk_love_is_color(-1)) {
		const char *msg = lua_pushfstring(L, "%%s: bad color string '%s'", lua_tostring(L, -1));
		nk_love_assert(0, msg);
	}
	struct nk_color color = nk_love_checkcolor(-1);
	int success = nk_style_push_color(&context, field, color);
	if (success) {
		lua_pushstring(L, "color");
		size_t stack_size = lua_objlen(L, 1);
		lua_rawseti(L, 1, stack_size + 1);
	}
	return success;
}

static int nk_love_style_push_vec2(struct nk_vec2 *field)
{
	static const char *msg = "%s: vec2 fields must have x and y components";
	nk_love_assert(lua_istable(L, -1), msg);
	lua_getfield(L, -1, "x");
	nk_love_assert(lua_isnumber(L, -1), msg);
	lua_getfield(L, -2, "y");
	nk_love_assert(lua_isnumber(L, -1), msg);
	struct nk_vec2 vec2;
	vec2.x = lua_tonumber(L, -2);
	vec2.y = lua_tonumber(L, -1);
	lua_pop(L, 2);
	int success = nk_style_push_vec2(&context, field, vec2);
	if (success) {
		lua_pushstring(L, "vec2");
		size_t stack_size = lua_objlen(L, 1);
		lua_rawseti(L, 1, stack_size + 1);
	}
	return success;
}

static int nk_love_style_push_item(struct nk_style_item *field)
{
	struct nk_style_item item;
	if (lua_isstring(L, -1)) {
		if (!nk_love_is_color(-1)) {
			const char *msg = lua_pushfstring(L, "%%s: bad color string '%s'", lua_tostring(L, -1));
			nk_love_assert(0, msg);
		}
		item.type = NK_STYLE_ITEM_COLOR;
		item.data.color = nk_love_checkcolor(-1);
	} else {
		item.type = NK_STYLE_ITEM_IMAGE;
		nk_love_checkImage(-1, &item.data.image);
	}
	int success = nk_style_push_style_item(&context, field, item);
	if (success) {
		lua_pushstring(L, "item");
		size_t stack_size = lua_objlen(L, 1);
		lua_rawseti(L, 1, stack_size + 1);
	}
	return success;
}

static int nk_love_style_push_align(nk_flags *field)
{
	nk_flags align = nk_love_checkalign(-1);
	int success = nk_style_push_flags(&context, field, align);
	if (success) {
		lua_pushstring(L, "flags");
		size_t stack_size = lua_objlen(L, 1);
		lua_rawseti(L, 1, stack_size + 1);
	}
	return success;
}

static int nk_love_style_push_float(float *field) {
	float f = luaL_checknumber(L, -1);
	int success = nk_style_push_float(&context, field, f);
	if (success) {
		lua_pushstring(L, "float");
		size_t stack_size = lua_objlen(L, 1);
		lua_rawseti(L, 1, stack_size + 1);
	}
	return success;
}

static int nk_love_style_push_font(const struct nk_user_font **field)
{
	nk_love_checkFont(-1, &fonts[font_count]);
	int success = nk_style_push_font(&context, &fonts[font_count++]);
	if (success) {
		lua_pushstring(L, "font");
		size_t stack_size = lua_objlen(L, 1);
		lua_rawseti(L, 1, stack_size + 1);
	}
	return success;
}

#define NK_LOVE_STYLE_PUSH(name, type, field) \
	nk_love_assert(lua_istable(L, -1), "%s: " name " field must be a table"); \
	lua_getfield(L, -1, name); \
	if (!lua_isnil(L, -1)) \
		nk_love_style_push_##type(field); \
	lua_pop(L, 1);

static void nk_love_style_push_text(struct nk_style_text *style)
{
	nk_love_assert(lua_istable(L, -1), "%s: text style must be a table");
	NK_LOVE_STYLE_PUSH("color", color, &style->color);
	NK_LOVE_STYLE_PUSH("padding", vec2, &style->padding);
}

static void nk_love_style_push_button(struct nk_style_button *style)
{
	nk_love_assert(lua_istable(L, -1), "%s: button style must be a table");
	NK_LOVE_STYLE_PUSH("normal", item, &style->normal);
	NK_LOVE_STYLE_PUSH("hover", item, &style->hover);
	NK_LOVE_STYLE_PUSH("active", item, &style->active);
	NK_LOVE_STYLE_PUSH("border color", color, &style->border_color);
	NK_LOVE_STYLE_PUSH("text background", color, &style->text_background);
	NK_LOVE_STYLE_PUSH("text normal", color, &style->text_normal);
	NK_LOVE_STYLE_PUSH("text hover", color, &style->text_hover);
	NK_LOVE_STYLE_PUSH("text active", color, &style->text_active);
	NK_LOVE_STYLE_PUSH("text alignment", align, &style->text_alignment);
	NK_LOVE_STYLE_PUSH("border", float, &style->border);
	NK_LOVE_STYLE_PUSH("rounding", float, &style->rounding);
	NK_LOVE_STYLE_PUSH("padding", vec2, &style->padding);
	NK_LOVE_STYLE_PUSH("image padding", vec2, &style->image_padding);
	NK_LOVE_STYLE_PUSH("touch padding", vec2, &style->touch_padding);
}

static void nk_love_style_push_scrollbar(struct nk_style_scrollbar *style)
{
	nk_love_assert(lua_istable(L, -1), "%s: scrollbar style must be a table");
	NK_LOVE_STYLE_PUSH("normal", item, &style->normal);
	NK_LOVE_STYLE_PUSH("hover", item, &style->hover);
	NK_LOVE_STYLE_PUSH("active", item, &style->active);
	NK_LOVE_STYLE_PUSH("border color", color, &style->border_color);
	NK_LOVE_STYLE_PUSH("cursor normal", item, &style->cursor_normal);
	NK_LOVE_STYLE_PUSH("cursor hover", item, &style->cursor_hover);
	NK_LOVE_STYLE_PUSH("cursor active", item, &style->active);
	NK_LOVE_STYLE_PUSH("cursor border color", color, &style->cursor_border_color);
	NK_LOVE_STYLE_PUSH("border", float, &style->border);
	NK_LOVE_STYLE_PUSH("rounding", float, &style->rounding);
	NK_LOVE_STYLE_PUSH("border cursor", float, &style->border_cursor);
	NK_LOVE_STYLE_PUSH("rounding cursor", float, &style->rounding_cursor);
	NK_LOVE_STYLE_PUSH("padding", vec2, &style->padding);
}

static void nk_love_style_push_edit(struct nk_style_edit *style)
{
	nk_love_assert(lua_istable(L, -1), "%s: edit style must be a table");
	NK_LOVE_STYLE_PUSH("normal", item, &style->normal);
	NK_LOVE_STYLE_PUSH("hover", item, &style->hover);
	NK_LOVE_STYLE_PUSH("active", item, &style->active);
	NK_LOVE_STYLE_PUSH("border color", color, &style->border_color);
	NK_LOVE_STYLE_PUSH("scrollbar", scrollbar, &style->scrollbar);
	NK_LOVE_STYLE_PUSH("cursor normal", color, &style->cursor_normal);
	NK_LOVE_STYLE_PUSH("cursor hover", color, &style->cursor_hover);
	NK_LOVE_STYLE_PUSH("cursor text normal", color, &style->cursor_text_normal);
	NK_LOVE_STYLE_PUSH("cursor text hover", color, &style->cursor_text_hover);
	NK_LOVE_STYLE_PUSH("text normal", color, &style->text_normal);
	NK_LOVE_STYLE_PUSH("text hover", color, &style->text_hover);
	NK_LOVE_STYLE_PUSH("text active", color, &style->text_active);
	NK_LOVE_STYLE_PUSH("selected normal", color, &style->selected_normal);
	NK_LOVE_STYLE_PUSH("selected hover", color, &style->selected_hover);
	NK_LOVE_STYLE_PUSH("selected text normal", color, &style->text_normal);
	NK_LOVE_STYLE_PUSH("selected text hover", color, &style->selected_text_hover);
	NK_LOVE_STYLE_PUSH("border", float, &style->border);
	NK_LOVE_STYLE_PUSH("rounding", float, &style->rounding);
	NK_LOVE_STYLE_PUSH("cursor size", float, &style->cursor_size);
	NK_LOVE_STYLE_PUSH("scrollbar size", vec2, &style->scrollbar_size);
	NK_LOVE_STYLE_PUSH("padding", vec2, &style->padding);
	NK_LOVE_STYLE_PUSH("row padding", float, &style->row_padding);
}

static void nk_love_style_push_toggle(struct nk_style_toggle *style)
{
	nk_love_assert(lua_istable(L, -1), "%s: toggle style must be a table");
	NK_LOVE_STYLE_PUSH("normal", item, &style->normal);
	NK_LOVE_STYLE_PUSH("hover", item, &style->hover);
	NK_LOVE_STYLE_PUSH("active", item, &style->active);
	NK_LOVE_STYLE_PUSH("border color", color, &style->border_color);
	NK_LOVE_STYLE_PUSH("cursor normal", item, &style->cursor_normal);
	NK_LOVE_STYLE_PUSH("cursor hover", item, &style->cursor_hover);
	NK_LOVE_STYLE_PUSH("text normal", color, &style->text_normal);
	NK_LOVE_STYLE_PUSH("text hover", color, &style->text_hover);
	NK_LOVE_STYLE_PUSH("text active", color, &style->text_active);
	NK_LOVE_STYLE_PUSH("text background", color, &style->text_background);
	NK_LOVE_STYLE_PUSH("text alignment", align, &style->text_alignment);
	NK_LOVE_STYLE_PUSH("padding", vec2, &style->padding);
	NK_LOVE_STYLE_PUSH("touch padding", vec2, &style->touch_padding);
	NK_LOVE_STYLE_PUSH("spacing", float, &style->spacing);
	NK_LOVE_STYLE_PUSH("border", float, &style->border);
}

static void nk_love_style_push_selectable(struct nk_style_selectable *style)
{
	nk_love_assert(lua_istable(L, -1), "%s: selectable style must be a table");
	NK_LOVE_STYLE_PUSH("normal", item, &style->normal);
	NK_LOVE_STYLE_PUSH("hover", item, &style->hover);
	NK_LOVE_STYLE_PUSH("pressed", item, &style->pressed);
	NK_LOVE_STYLE_PUSH("normal active", item, &style->normal_active);
	NK_LOVE_STYLE_PUSH("hover active", item, &style->hover_active);
	NK_LOVE_STYLE_PUSH("pressed active", item, &style->pressed_active);
	NK_LOVE_STYLE_PUSH("text normal", color, &style->text_normal);
	NK_LOVE_STYLE_PUSH("text hover", color, &style->text_hover);
	NK_LOVE_STYLE_PUSH("text pressed", color, &style->text_pressed);
	NK_LOVE_STYLE_PUSH("text normal active", color, &style->text_normal_active);
	NK_LOVE_STYLE_PUSH("text hover active", color, &style->text_hover_active);
	NK_LOVE_STYLE_PUSH("text pressed active", color, &style->text_pressed_active);
	NK_LOVE_STYLE_PUSH("text background", color, &style->text_background);
	NK_LOVE_STYLE_PUSH("text alignment", align, &style->text_alignment);
	NK_LOVE_STYLE_PUSH("rounding", float, &style->rounding);
	NK_LOVE_STYLE_PUSH("padding", vec2, &style->padding);
	NK_LOVE_STYLE_PUSH("touch padding", vec2, &style->touch_padding);
	NK_LOVE_STYLE_PUSH("image padding", vec2, &style->image_padding);
}

static void nk_love_style_push_slider(struct nk_style_slider *style)
{
	nk_love_assert(lua_istable(L, -1), "%s: slider style must be a table");
	NK_LOVE_STYLE_PUSH("normal", item, &style->normal);
	NK_LOVE_STYLE_PUSH("hover", item, &style->hover);
	NK_LOVE_STYLE_PUSH("active", item, &style->active);
	NK_LOVE_STYLE_PUSH("border color", color, &style->border_color);
	NK_LOVE_STYLE_PUSH("bar normal", color, &style->bar_normal);
	NK_LOVE_STYLE_PUSH("bar active", color, &style->bar_active);
	NK_LOVE_STYLE_PUSH("bar filled", color, &style->bar_filled);
	NK_LOVE_STYLE_PUSH("cursor normal", item, &style->cursor_normal);
	NK_LOVE_STYLE_PUSH("cursor hover", item, &style->cursor_hover);
	NK_LOVE_STYLE_PUSH("cursor active", item, &style->cursor_active);
	NK_LOVE_STYLE_PUSH("border", float, &style->border);
	NK_LOVE_STYLE_PUSH("rounding", float, &style->rounding);
	NK_LOVE_STYLE_PUSH("bar height", float, &style->bar_height);
	NK_LOVE_STYLE_PUSH("padding", vec2, &style->padding);
	NK_LOVE_STYLE_PUSH("spacing", vec2, &style->spacing);
	NK_LOVE_STYLE_PUSH("cursor size", vec2, &style->cursor_size);
}

static void nk_love_style_push_progress(struct nk_style_progress *style)
{
	nk_love_assert(lua_istable(L, -1), "%s: progress style must be a table");
	NK_LOVE_STYLE_PUSH("normal", item, &style->normal);
	NK_LOVE_STYLE_PUSH("hover", item, &style->hover);
	NK_LOVE_STYLE_PUSH("active", item, &style->active);
	NK_LOVE_STYLE_PUSH("border color", color, &style->border_color);
	NK_LOVE_STYLE_PUSH("cursor normal", item, &style->cursor_normal);
	NK_LOVE_STYLE_PUSH("cursor hover", item, &style->cursor_hover);
	NK_LOVE_STYLE_PUSH("cusor active", item, &style->cursor_active);
	NK_LOVE_STYLE_PUSH("cursor border color", color, &style->cursor_border_color);
	NK_LOVE_STYLE_PUSH("rounding", float, &style->rounding);
	NK_LOVE_STYLE_PUSH("border", float, &style->border);
	NK_LOVE_STYLE_PUSH("cursor border", float, &style->cursor_border);
	NK_LOVE_STYLE_PUSH("cursor rounding", float, &style->cursor_rounding);
	NK_LOVE_STYLE_PUSH("padding", vec2, &style->padding);
}

static void nk_love_style_push_property(struct nk_style_property *style)
{
	nk_love_assert(lua_istable(L, -1), "%s: property style must be a table");
	NK_LOVE_STYLE_PUSH("normal", item, &style->normal);
	NK_LOVE_STYLE_PUSH("hover", item, &style->hover);
	NK_LOVE_STYLE_PUSH("active", item, &style->active);
	NK_LOVE_STYLE_PUSH("border color", color, &style->border_color);
	NK_LOVE_STYLE_PUSH("label normal", color, &style->label_normal);
	NK_LOVE_STYLE_PUSH("label hover", color, &style->label_hover);
	NK_LOVE_STYLE_PUSH("label active", color, &style->label_active);
	NK_LOVE_STYLE_PUSH("border", float, &style->border);
	NK_LOVE_STYLE_PUSH("rounding", float, &style->rounding);
	NK_LOVE_STYLE_PUSH("padding", vec2, &style->padding);
	NK_LOVE_STYLE_PUSH("edit", edit, &style->edit);
	NK_LOVE_STYLE_PUSH("inc button", button, &style->inc_button);
	NK_LOVE_STYLE_PUSH("dec button", button, &style->dec_button);
}

static void nk_love_style_push_chart(struct nk_style_chart *style)
{
	nk_love_assert(lua_istable(L, -1), "%s: chart style must be a table");
	NK_LOVE_STYLE_PUSH("background", item, &style->background);
	NK_LOVE_STYLE_PUSH("border color", color, &style->border_color);
	NK_LOVE_STYLE_PUSH("selected color", color, &style->selected_color);
	NK_LOVE_STYLE_PUSH("color", color, &style->color);
	NK_LOVE_STYLE_PUSH("border", float, &style->border);
	NK_LOVE_STYLE_PUSH("rounding", float, &style->rounding);
	NK_LOVE_STYLE_PUSH("padding", vec2, &style->padding);
}

static void nk_love_style_push_tab(struct nk_style_tab *style)
{
	nk_love_assert(lua_istable(L, -1), "%s: tab style must be a table");
	NK_LOVE_STYLE_PUSH("background", item, &style->background);
	NK_LOVE_STYLE_PUSH("border color", color, &style->border_color);
	NK_LOVE_STYLE_PUSH("text", color, &style->text);
	NK_LOVE_STYLE_PUSH("tab maximize button", button, &style->tab_maximize_button);
	NK_LOVE_STYLE_PUSH("tab minimize button", button, &style->tab_minimize_button);
	NK_LOVE_STYLE_PUSH("node maximize button", button, &style->node_maximize_button);
	NK_LOVE_STYLE_PUSH("node minimize button", button, &style->node_minimize_button);
	NK_LOVE_STYLE_PUSH("border", float, &style->border);
	NK_LOVE_STYLE_PUSH("rounding", float, &style->rounding);
	NK_LOVE_STYLE_PUSH("indent", float, &style->indent);
	NK_LOVE_STYLE_PUSH("padding", vec2, &style->padding);
	NK_LOVE_STYLE_PUSH("spacing", vec2, &style->spacing);
}

static void nk_love_style_push_combo(struct nk_style_combo *style)
{
	nk_love_assert(lua_istable(L, -1), "%s: combo style must be a table");
	NK_LOVE_STYLE_PUSH("normal", item, &style->normal);
	NK_LOVE_STYLE_PUSH("hover", item, &style->hover);
	NK_LOVE_STYLE_PUSH("active", item, &style->active);
	NK_LOVE_STYLE_PUSH("border color", color, &style->border_color);
	NK_LOVE_STYLE_PUSH("label normal", color, &style->label_normal);
	NK_LOVE_STYLE_PUSH("label hover", color, &style->label_hover);
	NK_LOVE_STYLE_PUSH("label active", color, &style->label_active);
	NK_LOVE_STYLE_PUSH("symbol normal", color, &style->symbol_normal);
	NK_LOVE_STYLE_PUSH("symbol hover", color, &style->symbol_hover);
	NK_LOVE_STYLE_PUSH("symbol active", color, &style->symbol_active);
	NK_LOVE_STYLE_PUSH("button", button, &style->button);
	NK_LOVE_STYLE_PUSH("border", float, &style->border);
	NK_LOVE_STYLE_PUSH("rounding", float, &style->rounding);
	NK_LOVE_STYLE_PUSH("content padding", vec2, &style->content_padding);
	NK_LOVE_STYLE_PUSH("button padding", vec2, &style->button_padding);
	NK_LOVE_STYLE_PUSH("spacing", vec2, &style->spacing);
}

static void nk_love_style_push_window_header(struct nk_style_window_header *style)
{
	nk_love_assert(lua_istable(L, -1), "%s: window header style must be a table");
	NK_LOVE_STYLE_PUSH("normal", item, &style->normal);
	NK_LOVE_STYLE_PUSH("hover", item, &style->hover);
	NK_LOVE_STYLE_PUSH("active", item, &style->active);
	NK_LOVE_STYLE_PUSH("close button", button, &style->close_button);
	NK_LOVE_STYLE_PUSH("minimize button", button, &style->minimize_button);
	NK_LOVE_STYLE_PUSH("label normal", color, &style->label_normal);
	NK_LOVE_STYLE_PUSH("label hover", color, &style->label_hover);
	NK_LOVE_STYLE_PUSH("label active", color, &style->label_active);
	NK_LOVE_STYLE_PUSH("padding", vec2, &style->padding);
	NK_LOVE_STYLE_PUSH("label padding", vec2, &style->label_padding);
	NK_LOVE_STYLE_PUSH("spacing", vec2, &style->spacing);
}

static void nk_love_style_push_window(struct nk_style_window *style)
{
	nk_love_assert(lua_istable(L, -1), "%s: window style must be a table");
	NK_LOVE_STYLE_PUSH("header", window_header, &style->header);
	NK_LOVE_STYLE_PUSH("fixed background", item, &style->fixed_background);
	NK_LOVE_STYLE_PUSH("background", color, &style->background);
	NK_LOVE_STYLE_PUSH("border color", color, &style->border_color);
	NK_LOVE_STYLE_PUSH("popup border color", color, &style->popup_border_color);
	NK_LOVE_STYLE_PUSH("combo border color", color, &style->combo_border_color);
	NK_LOVE_STYLE_PUSH("contextual border color", color, &style->contextual_border_color);
	NK_LOVE_STYLE_PUSH("menu border color", color, &style->menu_border_color);
	NK_LOVE_STYLE_PUSH("group border color", color, &style->group_border_color);
	NK_LOVE_STYLE_PUSH("tooltip border color", color, &style->tooltip_border_color);
	NK_LOVE_STYLE_PUSH("scaler", item, &style->scaler);
	NK_LOVE_STYLE_PUSH("border", float, &style->border);
	NK_LOVE_STYLE_PUSH("combo border", float, &style->combo_border);
	NK_LOVE_STYLE_PUSH("contextual border", float, &style->contextual_border);
	NK_LOVE_STYLE_PUSH("menu border", float, &style->menu_border);
	NK_LOVE_STYLE_PUSH("group border", float, &style->group_border);
	NK_LOVE_STYLE_PUSH("tooltip border", float, &style->tooltip_border);
	NK_LOVE_STYLE_PUSH("popup border", float, &style->popup_border);
	NK_LOVE_STYLE_PUSH("rounding", float, &style->rounding);
	NK_LOVE_STYLE_PUSH("spacing", vec2, &style->spacing);
	NK_LOVE_STYLE_PUSH("scrollbar size", vec2, &style->scrollbar_size);
	NK_LOVE_STYLE_PUSH("min size", vec2, &style->min_size);
	NK_LOVE_STYLE_PUSH("padding", vec2, &style->padding);
	NK_LOVE_STYLE_PUSH("group padding", vec2, &style->group_padding);
	NK_LOVE_STYLE_PUSH("popup padding", vec2, &style->popup_padding);
	NK_LOVE_STYLE_PUSH("combo padding", vec2, &style->combo_padding);
	NK_LOVE_STYLE_PUSH("contextual padding", vec2, &style->contextual_padding);
	NK_LOVE_STYLE_PUSH("menu padding", vec2, &style->menu_padding);
	NK_LOVE_STYLE_PUSH("tooltip padding", vec2, &style->tooltip_padding);
}

static int nk_love_style_push(lua_State *L)
{
	nk_love_assert_argc(lua_gettop(L) == 1);
	if (!lua_istable(L, 1))
		luaL_typerror(L, 1, "table");
	lua_newtable(L);
	lua_insert(L, 1);
	NK_LOVE_STYLE_PUSH("font", font, &context.style.font);
	NK_LOVE_STYLE_PUSH("text", text, &context.style.text);
	NK_LOVE_STYLE_PUSH("button", button, &context.style.button);
	NK_LOVE_STYLE_PUSH("contextual button", button, &context.style.contextual_button);
	NK_LOVE_STYLE_PUSH("menu button", button, &context.style.menu_button);
	NK_LOVE_STYLE_PUSH("option", toggle, &context.style.option);
	NK_LOVE_STYLE_PUSH("checkbox", toggle, &context.style.checkbox);
	NK_LOVE_STYLE_PUSH("selectable", selectable, &context.style.selectable);
	NK_LOVE_STYLE_PUSH("slider", slider, &context.style.slider);
	NK_LOVE_STYLE_PUSH("progress", progress, &context.style.progress);
	NK_LOVE_STYLE_PUSH("property", property, &context.style.property);
	NK_LOVE_STYLE_PUSH("edit", edit, &context.style.edit);
	NK_LOVE_STYLE_PUSH("chart", chart, &context.style.chart);
	NK_LOVE_STYLE_PUSH("scrollh", scrollbar, &context.style.scrollh);
	NK_LOVE_STYLE_PUSH("scrollv", scrollbar, &context.style.scrollv);
	NK_LOVE_STYLE_PUSH("tab", tab, &context.style.tab);
	NK_LOVE_STYLE_PUSH("combo", combo, &context.style.combo);
	NK_LOVE_STYLE_PUSH("window", window, &context.style.window);
	lua_pop(L, 1);
	lua_getfield(L, LUA_REGISTRYINDEX, "nuklear");
	lua_getfield(L, -1, "stack");
	size_t stack_size = lua_objlen(L, -1);
	lua_pushvalue(L, 1);
	lua_rawseti(L, -2, stack_size + 1);
	return 0;
}

static int nk_love_style_pop(lua_State *L)
{
	nk_love_assert_argc(lua_gettop(L) == 0);
	lua_getfield(L, LUA_REGISTRYINDEX, "nuklear");
	lua_getfield(L, -1, "stack");
	size_t stack_size = lua_objlen(L, -1);
	lua_rawgeti(L, -1, stack_size);
	lua_pushnil(L);
	lua_rawseti(L, -3, stack_size);
	stack_size = lua_objlen(L, -1);
	size_t i;
	for (i = stack_size; i > 0; --i) {
		lua_rawgeti(L, -1, i);
		const char *type = lua_tostring(L, -1);
		if (!strcmp(type, "color")) {
			nk_style_pop_color(&context);
		} else if (!strcmp(type, "vec2")) {
			nk_style_pop_vec2(&context);
		} else if (!strcmp(type, "item")) {
			nk_style_pop_style_item(&context);
		} else if (!strcmp(type, "flags")) {
			nk_style_pop_flags(&context);
		} else if (!strcmp(type, "float")) {
			nk_style_pop_float(&context);
		} else if (!strcmp(type, "font")) {
			nk_style_pop_font(&context);
		} else {
			const char *msg = lua_pushfstring(L, "%%s: bad style item type '%s'", lua_tostring(L, -1));
			nk_love_assert(0, msg);
		}
		lua_pop(L, 1);
	}
	return 0;
}

static int nk_love_widget_bounds(lua_State *L)
{
	nk_love_assert_argc(lua_gettop(L) == 0);
	struct nk_rect bounds = nk_widget_bounds(&context);
	lua_pushnumber(L, bounds.x);
	lua_pushnumber(L, bounds.y);
	lua_pushnumber(L, bounds.w);
	lua_pushnumber(L, bounds.h);
	return 4;
}

static int nk_love_widget_position(lua_State *L)
{
	nk_love_assert_argc(lua_gettop(L) == 0);
	struct nk_vec2 pos = nk_widget_position(&context);
	lua_pushnumber(L, pos.x);
	lua_pushnumber(L, pos.y);
	return 2;
}

static int nk_love_widget_size(lua_State *L)
{
	nk_love_assert_argc(lua_gettop(L) == 0);
	struct nk_vec2 pos = nk_widget_size(&context);
	lua_pushnumber(L, pos.x);
	lua_pushnumber(L, pos.y);
	return 2;
}

static int nk_love_widget_width(lua_State *L)
{
	nk_love_assert_argc(lua_gettop(L) == 0);
	float width = nk_widget_width(&context);
	lua_pushnumber(L, width);
	return 1;
}

static int nk_love_widget_height(lua_State *L)
{
	nk_love_assert_argc(lua_gettop(L) == 0);
	float height = nk_widget_height(&context);
	lua_pushnumber(L, height);
	return 1;
}

static int nk_love_widget_is_hovered(lua_State *L)
{
	nk_love_assert_argc(lua_gettop(L) == 0);
	int hovered = nk_widget_is_hovered(&context);
	lua_pushboolean(L, hovered);
	return 1;
}

static int nk_love_widget_is_mouse_clicked(lua_State *L)
{
	int argc = lua_gettop(L);
	nk_love_assert_argc(argc >= 0 && argc <= 1);
	enum nk_buttons button = NK_BUTTON_LEFT;
	if (argc >= 1 && !lua_isnil(L, 1))
		button = nk_love_checkbutton(1);
	int clicked = (context.active == context.current) &&
			nk_input_is_mouse_pressed(&context.input, button);
	lua_pushboolean(L, clicked);
	return 1;
}

static int nk_love_widget_has_mouse_click(lua_State *L)
{
	int argc = lua_gettop(L);
	nk_love_assert_argc(argc >= 0 && argc <= 2);
	enum nk_buttons button = NK_BUTTON_LEFT;
	if (argc >= 1 && !lua_isnil(L, 1))
		button = nk_love_checkbutton(1);
	int down = 1;
	if (argc >= 2 && !lua_isnil(L, 2))
		down = nk_love_checkboolean(L, 2);
	int has_click = nk_widget_has_mouse_click_down(&context, button, down);
	lua_pushboolean(L, has_click);
	return 1;
}

static int nk_love_widget_has_mouse(lua_State *L, int down) {
	int argc = lua_gettop(L);
	nk_love_assert_argc(argc >= 0 && argc <= 1);
	enum nk_buttons button = NK_BUTTON_LEFT;
	if (argc >= 1 && !lua_isnil(L, 1))
		button = nk_love_checkbutton(1);
	int ret = nk_widget_has_mouse_click_down(&context, button, down);
	lua_pushboolean(L, ret);
	return 1;
}

static int nk_love_widget_has_mouse_pressed(lua_State *L)
{
	return nk_love_widget_has_mouse(L, nk_true);
}

static int nk_love_widget_has_mouse_released(lua_State *L)
{
	return nk_love_widget_has_mouse(L, nk_false);
}

static int nk_love_widget_is_mouse(lua_State *L, int down) {
	int argc = lua_gettop(L);
	nk_love_assert_argc(argc >= 0 && argc <= 1);
	enum nk_buttons button = NK_BUTTON_LEFT;
	if (argc >= 1 && !lua_isnil(L, 1))
		button = nk_love_checkbutton(1);
	struct nk_rect bounds = nk_widget_bounds(&context);
	int ret = nk_input_is_mouse_click_down_in_rect(&context.input, button, bounds, down);
	lua_pushboolean(L, ret);
	return 1;
}

static int nk_love_widget_is_mouse_pressed(lua_State *L)
{
	return nk_love_widget_is_mouse(L, nk_true);
}

static int nk_love_widget_is_mouse_released(lua_State *L)
{
	return nk_love_widget_is_mouse(L, nk_false);
}

static int nk_love_spacing(lua_State *L)
{
	nk_love_assert_argc(lua_gettop(L) == 1);
	int cols = luaL_checkint(L, 1);
	nk_spacing(&context, cols);
	return 0;
}

static int nk_love_line(lua_State *L)
{
	int argc = lua_gettop(L);
	nk_love_assert_argc(argc >= 4 && argc % 2 == 0);
	int i;
	for (i = 0; i < argc; ++i) {
		nk_love_assert(lua_isnumber(L, i + 1), "%s: point coordinates should be numbers");
		floats[i] = lua_tonumber(L, i + 1);
	}
	float line_thickness;
	struct nk_color color;
	nk_love_getGraphics(&line_thickness, &color);
	nk_stroke_polyline(&context.current->buffer, floats, argc / 2, line_thickness, color);
	return 0;
}

static int nk_love_curve(lua_State *L)
{
	nk_love_assert_argc(lua_gettop(L) == 8);
	int i;
	float ax = luaL_checknumber(L, 1);
	float ay = luaL_checknumber(L, 2);
	float ctrl0x = luaL_checknumber(L, 3);
	float ctrl0y = luaL_checknumber(L, 4);
	float ctrl1x = luaL_checknumber(L, 5);
	float ctrl1y = luaL_checknumber(L, 6);
	float bx = luaL_checknumber(L, 7);
	float by = luaL_checknumber(L, 8);
	float line_thickness;
	struct nk_color color;
	nk_love_getGraphics(&line_thickness, &color);
	nk_stroke_curve(&context.current->buffer, ax, ay, ctrl0x, ctrl0y, ctrl1x, ctrl1y, bx, by, line_thickness, color);
	return 0;
}

static int nk_love_polygon(lua_State *L)
{
	int argc = lua_gettop(L);
	nk_love_assert_argc(argc >= 7 && argc % 2 == 1);
	enum nk_love_draw_mode mode = nk_love_checkdraw(1);
	int i;
	for (i = 0; i < argc - 1; ++i) {
		nk_love_assert(lua_isnumber(L, i + 2), "%s: point coordinates should be numbers");
		floats[i] = lua_tonumber(L, i + 2);
	}
	float line_thickness;
	struct nk_color color;
	nk_love_getGraphics(&line_thickness, &color);
	if (mode == NK_LOVE_FILL)
		nk_fill_polygon(&context.current->buffer, floats, (argc - 1) / 2, color);
	else if (mode == NK_LOVE_LINE)
		nk_stroke_polygon(&context.current->buffer, floats, (argc - 1) / 2, line_thickness, color);
	return 0;
}

static int nk_love_circle(lua_State *L)
{
	nk_love_assert_argc(lua_gettop(L) == 4);
	enum nk_love_draw_mode mode = nk_love_checkdraw(1);
	float x = luaL_checknumber(L, 2);
	float y = luaL_checknumber(L, 3);
	float r = luaL_checknumber(L, 4);
	float line_thickness;
	struct nk_color color;
	nk_love_getGraphics(&line_thickness, &color);
	if (mode == NK_LOVE_FILL)
		nk_fill_circle(&context.current->buffer, nk_rect(x - r, y - r, r * 2, r * 2), color);
	else if (mode == NK_LOVE_LINE)
		nk_stroke_circle(&context.current->buffer, nk_rect(x - r, y - r, r * 2, r * 2), line_thickness, color);
	return 0;
}

static int nk_love_ellipse(lua_State *L)
{
	nk_love_assert_argc(lua_gettop(L) == 5);
	enum nk_love_draw_mode mode = nk_love_checkdraw(1);
	float x = luaL_checknumber(L, 2);
	float y = luaL_checknumber(L, 3);
	float rx = luaL_checknumber(L, 4);
	float ry = luaL_checknumber(L, 5);
	float line_thickness;
	struct nk_color color;
	nk_love_getGraphics(&line_thickness, &color);
	if (mode == NK_LOVE_FILL)
		nk_fill_circle(&context.current->buffer, nk_rect(x - rx, y - ry, rx * 2, ry * 2), color);
	else if (mode == NK_LOVE_LINE)
		nk_stroke_circle(&context.current->buffer, nk_rect(x - rx, y - ry, rx * 2, ry * 2), line_thickness, color);
	return 0;
}

static int nk_love_arc(lua_State *L)
{
	nk_love_assert_argc(lua_gettop(L) == 6);
	enum nk_love_draw_mode mode = nk_love_checkdraw(1);
	float cx = luaL_checknumber(L, 2);
	float cy = luaL_checknumber(L, 3);
	float r = luaL_checknumber(L, 4);
	float a0 = luaL_checknumber(L, 5);
	float a1 = luaL_checknumber(L, 6);
	float line_thickness;
	struct nk_color color;
	nk_love_getGraphics(&line_thickness, &color);
	if (mode == NK_LOVE_FILL)
		nk_fill_arc(&context.current->buffer, cx, cy, r, a0, a1, color);
	else if (mode == NK_LOVE_LINE)
		nk_stroke_arc(&context.current->buffer, cx, cy, r, a0, a1, line_thickness, color);
	return 0;
}

static int nk_love_rect_multi_color(lua_State *L)
{
	nk_love_assert_argc(lua_gettop(L) == 8);
	float x = luaL_checknumber(L, 1);
	float y = luaL_checknumber(L, 2);
	float w = luaL_checknumber(L, 3);
	float h = luaL_checknumber(L, 4);
	struct nk_color topLeft = nk_love_checkcolor(5);
	struct nk_color topRight = nk_love_checkcolor(6);
	struct nk_color bottomLeft = nk_love_checkcolor(7);
	struct nk_color bottomRight = nk_love_checkcolor(8);
	nk_fill_rect_multi_color(&context.current->buffer, nk_rect(x, y, w, h), topLeft, topRight, bottomLeft, bottomRight);
	return 0;
}

static int nk_love_push_scissor(lua_State *L)
{
	nk_love_assert_argc(lua_gettop(L) == 4);
	float x = luaL_checknumber(L, 1);
	float y = luaL_checknumber(L, 2);
	float w = luaL_checknumber(L, 3);
	float h = luaL_checknumber(L, 4);
	nk_push_scissor(&context.current->buffer, nk_rect(x, y, w, h));
	return 0;
}

static int nk_love_text(lua_State *L)
{
	nk_love_assert_argc(lua_gettop(L) == 5);
	const char *text = luaL_checkstring(L, 1);
	float x = luaL_checknumber(L, 2);
	float y = luaL_checknumber(L, 3);
	float w = luaL_checknumber(L, 4);
	float h = luaL_checknumber(L, 5);
	lua_getglobal(L, "love");
	lua_getfield(L, -1, "graphics");
	lua_getfield(L, -1, "getFont");
	lua_call(L, 0, 1);
	nk_love_checkFont(-1, &fonts[font_count]);
	float line_thickness;
	struct nk_color color;
	nk_love_getGraphics(&line_thickness, &color);
	nk_draw_text(&context.current->buffer, nk_rect(x, y, w, h), text, strlen(text), &fonts[font_count++], nk_rgba(0, 0, 0, 0), color);
	return 0;
}

static int nk_love_input_has_mouse(int down)
{
	int argc = lua_gettop(L);
	nk_love_assert_argc(argc == 5);
	enum nk_buttons button = nk_love_checkbutton(1);
	float x = luaL_checknumber(L, 2);
	float y = luaL_checknumber(L, 3);
	float w = luaL_checknumber(L, 4);
	float h = luaL_checknumber(L, 5);
	int ret = nk_input_has_mouse_click_down_in_rect(&context.input, button, nk_rect(x, y, w, h), down);
	lua_pushboolean(L, ret);
	return 1;
}

static int nk_love_input_has_mouse_pressed(lua_State *L)
{
	return nk_love_input_has_mouse(nk_true);
}

static int nk_love_input_has_mouse_released(lua_State *L)
{
	return nk_love_input_has_mouse(nk_false);
}

static int nk_love_input_is_mouse(int down)
{
	int argc = lua_gettop(L);
	nk_love_assert_argc(argc == 5);
	enum nk_buttons button = nk_love_checkbutton(1);
	float x = luaL_checknumber(L, 2);
	float y = luaL_checknumber(L, 3);
	float w = luaL_checknumber(L, 4);
	float h = luaL_checknumber(L, 5);
	int ret = nk_input_is_mouse_click_down_in_rect(&context.input, button, nk_rect(x, y, w, h), down);
	lua_pushboolean(L, ret);
	return 1;
}

static int nk_love_input_is_mouse_pressed(lua_State *L)
{
	nk_love_input_is_mouse(nk_true);
}

static int nk_love_input_is_mouse_released(lua_State *L)
{
	nk_love_input_is_mouse(nk_false);
}

static int nk_love_input_was_hovered(lua_State *L)
{
	nk_love_assert_argc(lua_gettop(L) == 4);
	float x = luaL_checknumber(L, 1);
	float y = luaL_checknumber(L, 2);
	float w = luaL_checknumber(L, 3);
	float h = luaL_checknumber(L, 4);
	int was_hovered = nk_input_is_mouse_prev_hovering_rect(&context.input, nk_rect(x, y, w, h));
	lua_pushboolean(L, was_hovered);
	return 1;
}

static int nk_love_input_is_hovered(lua_State *L)
{
	nk_love_assert_argc(lua_gettop(L) == 4);
	float x = luaL_checknumber(L, 1);
	float y = luaL_checknumber(L, 2);
	float w = luaL_checknumber(L, 3);
	float h = luaL_checknumber(L, 4);
	int is_hovered = nk_input_is_mouse_hovering_rect(&context.input, nk_rect(x, y, w, h));
	lua_pushboolean(L, is_hovered);
	return 1;
}

// List of functions to wrap.
static const luaL_Reg functions[] =
{
	{"init", nk_love_init},
	{"shutdown", nk_love_shutdown},

	{"keypressed", nk_love_keypressed},
	{"keyreleased", nk_love_keyreleased},
	{"mousepressed", nk_love_mousepressed},
	{"mousereleased", nk_love_mousereleased},
	{"mousemoved", nk_love_mousemoved},
	{"textinput", nk_love_textinput},
	{"wheelmoved", nk_love_wheelmoved},

	{"draw", nk_love_draw},

	{"frame_begin", nk_love_frame_begin},
	{"frameBegin", nk_love_frame_begin},
	{"frame_end", nk_love_frame_end},
	{"frameEnd", nk_love_frame_end},

	{"window_begin", nk_love_window_begin},
	{"windowBegin", nk_love_window_begin},
	{"window_end", nk_love_window_end},
	{"windowEnd", nk_love_window_end},
	{"window_get_bounds", nk_love_window_get_bounds},
	{"windowGetBounds", nk_love_window_get_bounds},
	{"window_get_position", nk_love_window_get_position},
	{"windowGetPosition", nk_love_window_get_position},
	{"window_get_size", nk_love_window_get_size},
	{"windowGetSize", nk_love_window_get_size},
	{"window_get_content_region", nk_love_window_get_content_region},
	{"windowGetContentRegion", nk_love_window_get_content_region},
	{"window_has_focus", nk_love_window_has_focus},
	{"windowHasFocus", nk_love_window_has_focus},
	{"window_is_collapsed", nk_love_window_is_collapsed},
	{"windowIsCollapsed", nk_love_window_is_collapsed},
	{"window_is_hidden", nk_love_window_is_hidden},
	{"windowIsHidden", nk_love_window_is_hidden},
	{"window_is_active", nk_love_window_is_active},
	{"windowIsActive", nk_love_window_is_active},
	{"window_is_hovered", nk_love_window_is_hovered},
	{"windowIsHovered", nk_love_window_is_hovered},
	{"window_is_any_hovered", nk_love_window_is_any_hovered},
	{"windowIsAnyHovered", nk_love_window_is_any_hovered},
	{"item_is_any_active", nk_love_item_is_any_active},
	{"itemIsAnyActive", nk_love_item_is_any_active},
	{"window_set_bounds", nk_love_window_set_bounds},
	{"windowSetBounds", nk_love_window_set_bounds},
	{"window_set_position", nk_love_window_set_position},
	{"windowSetPosition", nk_love_window_set_position},
	{"window_set_size", nk_love_window_set_size},
	{"windowSetSize", nk_love_window_set_size},
	{"window_set_focus", nk_love_window_set_focus},
	{"windowSetFocus", nk_love_window_set_focus},
	{"window_close", nk_love_window_close},
	{"windowClose", nk_love_window_close},
	{"window_collapse", nk_love_window_collapse},
	{"windowCollapse", nk_love_window_collapse},
	{"window_expand", nk_love_window_expand},
	{"windowExpand", nk_love_window_expand},
	{"window_show", nk_love_window_show},
	{"windowShow", nk_love_window_show},
	{"window_hide", nk_love_window_hide},
	{"windowHide", nk_love_window_hide},

	{"layout_row", nk_love_layout_row},
	{"layoutRow", nk_love_layout_row},
	{"layout_row_begin", nk_love_layout_row_begin},
	{"layoutRowBegin", nk_love_layout_row_begin},
	{"layout_row_push", nk_love_layout_row_push},
	{"layoutRowPush", nk_love_layout_row_push},
	{"layout_row_end", nk_love_layout_row_end},
	{"layoutRowEnd", nk_love_layout_row_end},
	{"layout_space_begin", nk_love_layout_space_begin},
	{"layoutSpaceBegin", nk_love_layout_space_begin},
	{"layout_space_push", nk_love_layout_space_push},
	{"layoutSpacePush", nk_love_layout_space_push},
	{"layout_space_end", nk_love_layout_space_end},
	{"layoutSpaceEnd", nk_love_layout_space_end},
	{"layout_space_bounds", nk_love_layout_space_bounds},
	{"layoutSpaceBounds", nk_love_layout_space_bounds},
	{"layout_space_to_screen", nk_love_layout_space_to_screen},
	{"layoutSpaceToScreen", nk_love_layout_space_to_screen},
	{"layout_space_to_local", nk_love_layout_space_to_local},
	{"layoutSpaceToLocal", nk_love_layout_space_to_local},
	{"layout_space_rect_to_screen", nk_love_layout_space_rect_to_screen},
	{"layoutSpaceRectToScreen", nk_love_layout_space_rect_to_screen},
	{"layout_space_rect_to_local", nk_love_layout_space_rect_to_local},
	{"layoutSpaceRectToLocal", nk_love_layout_space_rect_to_local},
	{"layout_ratio_from_pixel", nk_love_layout_ratio_from_pixel},
	{"layoutRatioFromPixel", nk_love_layout_ratio_from_pixel},

	{"group_begin", nk_love_group_begin},
	{"groupBegin", nk_love_group_begin},
	{"group_end", nk_love_group_end},
	{"groupEnd", nk_love_group_end},

	{"tree_push", nk_love_tree_push},
	{"treePush", nk_love_tree_push},
	{"tree_pop", nk_love_tree_pop},
	{"treePop", nk_love_tree_pop},

	{"color_rgba", nk_love_color_rgba},
	{"colorRGBA", nk_love_color_rgba},
	{"color_hsva", nk_love_color_hsva},
	{"colorHSVA", nk_love_color_hsva},
	{"color_parse_rgba", nk_love_color_parse_rgba},
	{"colorParseRGBA", nk_love_color_parse_rgba},
	{"color_parse_hsva", nk_love_color_parse_hsva},
	{"colorParseHSVA", nk_love_color_parse_hsva},

	{"label", nk_love_label},
	{"image", nk_love_image},
	{"button", nk_love_button},
	{"button_set_behavior", nk_love_button_set_behavior},
	{"buttonSetBehavior", nk_love_button_set_behavior},
	{"button_push_behavior", nk_love_button_push_behavior},
	{"buttonPushBehavior", nk_love_button_push_behavior},
	{"button_pop_behavior", nk_love_button_pop_behavior},
	{"buttonPopBehavior", nk_love_button_pop_behavior},
	{"checkbox", nk_love_checkbox},
	{"radio", nk_love_radio},
	{"selectable", nk_love_selectable},
	{"slider", nk_love_slider},
	{"progress", nk_love_progress},
	{"color_picker", nk_love_color_picker},
	{"colorPicker", nk_love_color_picker},
	{"property", nk_love_property},
	{"edit", nk_love_edit},
	{"popup_begin", nk_love_popup_begin},
	{"popupBegin", nk_love_popup_begin},
	{"popup_close", nk_love_popup_close},
	{"popupClose", nk_love_popup_close},
	{"popup_end", nk_love_popup_end},
	{"popupEnd", nk_love_popup_end},
	{"combobox", nk_love_combobox},
	{"combobox_begin", nk_love_combobox_begin},
	{"comboboxBegin", nk_love_combobox_begin},
	{"combobox_item", nk_love_combobox_item},
	{"comboboxItem", nk_love_combobox_item},
	{"combobox_close", nk_love_combobox_close},
	{"comboboxClose", nk_love_combobox_close},
	{"combobox_end", nk_love_combobox_end},
	{"comboboxEnd", nk_love_combobox_end},
	{"contextual_begin", nk_love_contextual_begin},
	{"contextualBegin", nk_love_contextual_begin},
	{"contextual_item", nk_love_contextual_item},
	{"contextualItem", nk_love_contextual_item},
	{"contextual_close", nk_love_contextual_close},
	{"contextualClose", nk_love_contextual_close},
	{"contextual_end", nk_love_contextual_end},
	{"contextualEnd", nk_love_contextual_end},
	{"tooltip", nk_love_tooltip},
	{"tooltip_begin", nk_love_tooltip_begin},
	{"tooltipBegin", nk_love_tooltip_begin},
	{"tooltip_end", nk_love_tooltip_end},
	{"tooltipEnd", nk_love_tooltip_end},
	{"menubar_begin", nk_love_menubar_begin},
	{"menubarBegin", nk_love_menubar_begin},
	{"menubar_end", nk_love_menubar_end},
	{"menubarEnd", nk_love_menubar_end},
	{"menu_begin", nk_love_menu_begin},
	{"menuBegin", nk_love_menu_begin},
	{"menu_item", nk_love_menu_item},
	{"menuItem", nk_love_menu_item},
	{"menu_close", nk_love_menu_close},
	{"menuClose", nk_love_menu_close},
	{"menu_end", nk_love_menu_end},
	{"menuEnd", nk_love_menu_end},

	{"style_default", nk_love_style_default},
	{"styleDefault", nk_love_style_default},
	{"style_load_colors", nk_love_style_load_colors},
	{"styleLoadColors", nk_love_style_load_colors},
	{"style_set_font", nk_love_style_set_font},
	{"styleSetFont", nk_love_style_set_font},
	{"style_push", nk_love_style_push},
	{"stylePush", nk_love_style_push},
	{"style_pop", nk_love_style_pop},
	{"stylePop", nk_love_style_pop},

	{"widget_bounds", nk_love_widget_bounds},
	{"widgetBounds", nk_love_widget_bounds},
	{"widget_position", nk_love_widget_position},
	{"widgetPosition", nk_love_widget_position},
	{"widget_size", nk_love_widget_size},
	{"widgetSize", nk_love_widget_size},
	{"widget_width", nk_love_widget_width},
	{"widgetWidth", nk_love_widget_width},
	{"widget_height", nk_love_widget_height},
	{"widgetHeight", nk_love_widget_height},
	{"widget_is_hovered", nk_love_widget_is_hovered},
	{"widgetIsHovered", nk_love_widget_is_hovered},
	{"widget_is_mouse_clicked", nk_love_widget_is_mouse_clicked},
	{"widgetIsMouseClicked", nk_love_widget_is_mouse_clicked},
	{"widget_has_mouse_click", nk_love_widget_has_mouse_click},
	{"widgetHasMouseClick", nk_love_widget_has_mouse_click},
	{"widgetHasMousePressed", nk_love_widget_has_mouse_pressed},
	{"widgetHasMouseReleased", nk_love_widget_has_mouse_released},
	{"widgetIsMousePressed", nk_love_widget_is_mouse_pressed},
	{"widgetIsMouseReleased", nk_love_widget_is_mouse_released},
	{"spacing", nk_love_spacing},

	{"line", nk_love_line},
	{"curve", nk_love_curve},
	{"polygon", nk_love_polygon},
	{"circle", nk_love_circle},
	{"ellipse", nk_love_ellipse},
	{"arc", nk_love_arc},
	{"rectMultiColor", nk_love_rect_multi_color},
	{"scissor", nk_love_push_scissor},
	/* image */
	{"text", nk_love_text},

	{"inputHasMousePressed", nk_love_input_has_mouse_pressed},
	{"inputHasMouseReleased", nk_love_input_has_mouse_released},
	{"inputIsMousePressed", nk_love_input_is_mouse_pressed},
	{"inputIsMouseReleased", nk_love_input_is_mouse_released},
	{"inputWasHovered", nk_love_input_was_hovered},
	{"inputIsHovered", nk_love_input_is_hovered},
	{ 0, 0 }
};

static const lua_CFunction types[] =
{
	0
};

extern "C" int luaopen_love_plugin_nuklear(lua_State *L)
{
	WrappedModule w;
	w.module = &nuklear::Nuklear::instance;
	w.name = "nuklear";
	w.type = &Module::type;
	w.functions = functions;
	w.types = types;

	return luax_register_module(L, w);
}

#undef NK_LOVE_REGISTER
