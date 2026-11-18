/* libjs/webapi.c - bounded modern byte/text and URL parameter APIs.
 *
 * These APIs are separate from the core language builtins because they model
 * browser-facing resources. URLSearchParams keeps alternating key/value
 * strings in one dense array; codecs operate directly on ArrayBuffer and
 * Uint8Array byte spans. All allocation is charged to the owning JS context.
 */

#define JS_INTERNAL
#include "js.h"

#include <string.h>

#define TEXTDEC_FATAL       0x01u
#define TEXTDEC_IGNORE_BOM  0x02u
#define URLPARAMS_MAX_PAIRS 1024u
#define URLPARAMS_MAX_INPUT (1024UL * 1024UL)

static js_value web_arg(int argc, js_value *argv, int index)
{
    return index < argc ? argv[index] : js_undefined();
}

static int web_method(js_ctx *ctx, js_object *object, const char *name,
                      js_native fn, int length)
{
    return js_define_native(ctx, object, name, fn, length) == JS_OK;
}

static js_object *web_constructor(js_ctx *ctx, js_native fn,
                                  const char *name, int length,
                                  js_object *proto)
{
    js_object *ctor =
        js_new_native_constructor(ctx, fn, name, length, proto);

    if (!ctor)
        return 0;
    if (js_define(ctx, ctx->global, name, js_object_value(ctor)) != JS_OK)
        return 0;
    return ctor;
}

static int web_option_bool(js_ctx *ctx, js_value options, const char *name,
                           int fallback, int *out)
{
    js_value value;

    *out = fallback;
    if (options.type == JS_UNDEFINED || options.type == JS_NULL)
        return JS_OK;
    if (js_get(ctx, options, name, &value) != JS_OK)
        return JS_THROW;
    if (value.type != JS_UNDEFINED)
        *out = js_to_boolean(value);
    return JS_OK;
}

static int web_return_string(js_ctx *ctx, const char *bytes,
                             unsigned long length, js_value *out)
{
    *out = js_mkstring(ctx, bytes, length);
    return out->type == JS_STRING ? JS_OK : JS_THROW;
}

/* Return a complete valid UTF-8 sequence length, or zero for an invalid
 * leading byte/continuation sequence. */
static unsigned int utf8_span(const unsigned char *s, unsigned long length)
{
    unsigned int c;

    if (!length)
        return 0;
    c = s[0];
    if (c < 0x80)
        return 1;
    if (c >= 0xC2 && c <= 0xDF)
        return length >= 2 && (s[1] & 0xC0) == 0x80 ? 2 : 0;
    if (c == 0xE0)
        return length >= 3 && s[1] >= 0xA0 && s[1] <= 0xBF &&
               (s[2] & 0xC0) == 0x80 ? 3 : 0;
    if ((c >= 0xE1 && c <= 0xEC) || (c >= 0xEE && c <= 0xEF))
        return length >= 3 && (s[1] & 0xC0) == 0x80 &&
               (s[2] & 0xC0) == 0x80 ? 3 : 0;
    if (c == 0xED)
        return length >= 3 && s[1] >= 0x80 && s[1] <= 0x9F &&
               (s[2] & 0xC0) == 0x80 ? 3 : 0;
    if (c == 0xF0)
        return length >= 4 && s[1] >= 0x90 && s[1] <= 0xBF &&
               (s[2] & 0xC0) == 0x80 && (s[3] & 0xC0) == 0x80 ? 4 : 0;
    if (c >= 0xF1 && c <= 0xF3)
        return length >= 4 && (s[1] & 0xC0) == 0x80 &&
               (s[2] & 0xC0) == 0x80 && (s[3] & 0xC0) == 0x80 ? 4 : 0;
    if (c == 0xF4)
        return length >= 4 && s[1] >= 0x80 && s[1] <= 0x8F &&
               (s[2] & 0xC0) == 0x80 && (s[3] & 0xC0) == 0x80 ? 4 : 0;
    return 0;
}

/* ================================================================== */
/* TextEncoder                                                         */
/* ================================================================== */

static js_object *text_encoder_this(js_ctx *ctx, js_value value)
{
    if (value.type != JS_OBJECT ||
        value.u.obj->cls != JS_CLASS_TEXTENCODER) {
        js_throw_error(ctx, JS_ERR_TYPE, "invalid TextEncoder receiver");
        return 0;
    }
    return value.u.obj;
}

static int text_encoder_ctor(js_ctx *ctx, js_value this_value, int argc,
                             js_value *argv, js_value *out)
{
    js_object *object;

    (void)this_value;
    (void)argc;
    (void)argv;
    if (!ctx->new_target)
        return js_throw_error(ctx, JS_ERR_TYPE,
                              "TextEncoder must be constructed with new");
    object = js_obj_alloc(ctx, JS_CLASS_TEXTENCODER,
                          ctx->proto[P_TEXTENCODER]);
    if (!object)
        return JS_THROW;
    *out = js_object_value(object);
    return JS_OK;
}

static int text_encoder_encoding(js_ctx *ctx, js_value this_value, int argc,
                                 js_value *argv, js_value *out)
{
    (void)argc;
    (void)argv;
    if (!text_encoder_this(ctx, this_value))
        return JS_THROW;
    return web_return_string(ctx, "utf-8", 5, out);
}

static int text_encoder_encode(js_ctx *ctx, js_value this_value, int argc,
                               js_value *argv, js_value *out)
{
    js_value source;

    if (!text_encoder_this(ctx, this_value))
        return JS_THROW;
    if (argc == 0) {
        *out = js_uint8array_new(ctx, 0, 0);
    } else {
        if (js_to_string(ctx, argv[0], &source) != JS_OK)
            return JS_THROW;
        *out = js_uint8array_new(ctx, source.u.str->data,
                                source.u.str->len);
    }
    return js_is_uint8array(*out) ? JS_OK : JS_THROW;
}

static int text_encoder_encode_into(js_ctx *ctx, js_value this_value,
                                    int argc, js_value *argv, js_value *out)
{
    js_value source;
    const unsigned char *input;
    unsigned char *dest;
    unsigned long input_length, dest_length;
    unsigned long read = 0, written = 0;
    js_object *result;

    if (!text_encoder_this(ctx, this_value))
        return JS_THROW;
    if (js_to_string(ctx, web_arg(argc, argv, 0), &source) != JS_OK)
        return JS_THROW;
    if (argc < 2 || !js_is_uint8array(argv[1]))
        return js_throw_error(ctx, JS_ERR_TYPE,
                              "TextEncoder.encodeInto needs a Uint8Array");
    input = (const unsigned char *)source.u.str->data;
    input_length = source.u.str->len;
    dest = (unsigned char *)js_uint8array_data(argv[1], &dest_length);
    while (read < input_length) {
        unsigned int span = utf8_span(input + read, input_length - read);

        if (!span)
            span = 1;
        if ((unsigned long)span > dest_length - written)
            break;
        memcpy(dest + written, input + read, span);
        read += span;
        written += span;
    }
    result = js_new_object(ctx);
    if (!result)
        return JS_THROW;
    *out = js_object_value(result);
    if (js_set(ctx, *out, "read", js_number((double)read)) != JS_OK ||
        js_set(ctx, *out, "written", js_number((double)written)) != JS_OK)
        return JS_THROW;
    return JS_OK;
}

/* ================================================================== */
/* TextDecoder                                                         */
/* ================================================================== */

static js_object *text_decoder_this(js_ctx *ctx, js_value value)
{
    if (value.type != JS_OBJECT ||
        value.u.obj->cls != JS_CLASS_TEXTDECODER) {
        js_throw_error(ctx, JS_ERR_TYPE, "invalid TextDecoder receiver");
        return 0;
    }
    return value.u.obj;
}

static int ascii_equal_fold(const char *a, unsigned long length,
                            const char *literal)
{
    unsigned long i;

    if (strlen(literal) != length)
        return 0;
    for (i = 0; i < length; i++) {
        int c = (unsigned char)a[i];
        int d = (unsigned char)literal[i];

        if (c >= 'A' && c <= 'Z')
            c += 'a' - 'A';
        if (c != d)
            return 0;
    }
    return 1;
}

static int text_decoder_label(js_ctx *ctx, js_value value)
{
    js_value label;
    const char *bytes;
    unsigned long length, begin = 0, end;

    if (value.type == JS_UNDEFINED)
        return 1;
    if (js_to_string(ctx, value, &label) != JS_OK)
        return -1;
    bytes = label.u.str->data;
    length = label.u.str->len;
    end = length;
    while (begin < end && (bytes[begin] == ' ' || bytes[begin] == '\t' ||
                           bytes[begin] == '\r' || bytes[begin] == '\n'))
        begin++;
    while (end > begin && (bytes[end - 1] == ' ' || bytes[end - 1] == '\t' ||
                           bytes[end - 1] == '\r' || bytes[end - 1] == '\n'))
        end--;
    if (ascii_equal_fold(bytes + begin, end - begin, "utf-8") ||
        ascii_equal_fold(bytes + begin, end - begin, "utf8") ||
        ascii_equal_fold(bytes + begin, end - begin, "unicode-1-1-utf-8"))
        return 1;
    js_throw_error(ctx, JS_ERR_RANGE, "TextDecoder supports only UTF-8");
    return -1;
}

static int text_decoder_ctor(js_ctx *ctx, js_value this_value, int argc,
                             js_value *argv, js_value *out)
{
    js_object *object;
    int fatal, ignore_bom;

    (void)this_value;
    if (!ctx->new_target)
        return js_throw_error(ctx, JS_ERR_TYPE,
                              "TextDecoder must be constructed with new");
    if (text_decoder_label(ctx, web_arg(argc, argv, 0)) < 0)
        return JS_THROW;
    if (web_option_bool(ctx, web_arg(argc, argv, 1), "fatal", 0,
                        &fatal) != JS_OK ||
        web_option_bool(ctx, web_arg(argc, argv, 1), "ignoreBOM", 0,
                        &ignore_bom) != JS_OK)
        return JS_THROW;
    object = js_obj_alloc(ctx, JS_CLASS_TEXTDECODER,
                          ctx->proto[P_TEXTDECODER]);
    if (!object)
        return JS_THROW;
    object->codec_flags = (uint8_t)((fatal ? TEXTDEC_FATAL : 0) |
                                    (ignore_bom ? TEXTDEC_IGNORE_BOM : 0));
    *out = js_object_value(object);
    return JS_OK;
}

static int text_decoder_encoding(js_ctx *ctx, js_value this_value, int argc,
                                 js_value *argv, js_value *out)
{
    (void)argc;
    (void)argv;
    if (!text_decoder_this(ctx, this_value))
        return JS_THROW;
    return web_return_string(ctx, "utf-8", 5, out);
}

static int text_decoder_fatal(js_ctx *ctx, js_value this_value, int argc,
                              js_value *argv, js_value *out)
{
    js_object *object = text_decoder_this(ctx, this_value);

    (void)argc;
    (void)argv;
    if (!object)
        return JS_THROW;
    *out = js_bool((object->codec_flags & TEXTDEC_FATAL) != 0);
    return JS_OK;
}

static int text_decoder_ignore_bom(js_ctx *ctx, js_value this_value, int argc,
                                   js_value *argv, js_value *out)
{
    js_object *object = text_decoder_this(ctx, this_value);

    (void)argc;
    (void)argv;
    if (!object)
        return JS_THROW;
    *out = js_bool((object->codec_flags & TEXTDEC_IGNORE_BOM) != 0);
    return JS_OK;
}

static int text_decoder_decode(js_ctx *ctx, js_value this_value, int argc,
                               js_value *argv, js_value *out)
{
    js_object *decoder = text_decoder_this(ctx, this_value);
    const unsigned char *input = 0;
    unsigned long length = 0, at = 0;
    js_sbuf result;

    if (!decoder)
        return JS_THROW;
    if (argc > 0 && argv[0].type != JS_UNDEFINED) {
        if (js_is_arraybuffer(argv[0])) {
            input = (const unsigned char *)
                js_arraybuffer_data(argv[0], &length);
        } else if (js_is_typedarray(argv[0]) || js_is_dataview(argv[0])) {
            js_object *view = argv[0].u.obj;
            input = (const unsigned char *)
                (view->view_buffer->buffer + view->byte_offset);
            length = view->byte_length;
        } else {
            return js_throw_error(ctx, JS_ERR_TYPE,
                                  "TextDecoder.decode needs a byte buffer");
        }
    }
    if (!(decoder->codec_flags & TEXTDEC_IGNORE_BOM) && length >= 3 &&
        input[0] == 0xEF && input[1] == 0xBB && input[2] == 0xBF)
        at = 3;
    js_sb_init(&result, ctx);
    while (at < length) {
        unsigned int span = utf8_span(input + at, length - at);

        if (!span) {
            static const char replacement[] = "\xEF\xBF\xBD";

            if (decoder->codec_flags & TEXTDEC_FATAL) {
                js_sb_free(&result);
                return js_throw_error(ctx, JS_ERR_TYPE,
                                      "invalid UTF-8 input");
            }
            if (js_sb_put(&result, replacement, 3) != JS_OK) {
                js_sb_free(&result);
                return JS_THROW;
            }
            at++;
            continue;
        }
        if (js_sb_put(&result, (const char *)input + at, span) != JS_OK) {
            js_sb_free(&result);
            return JS_THROW;
        }
        at += span;
    }
    return js_sb_finish(&result, out);
}

/* ================================================================== */
/* URLSearchParams                                                     */
/* ================================================================== */

int js_is_urlsearchparams(js_value value)
{
    return value.type == JS_OBJECT &&
           value.u.obj->cls == JS_CLASS_URLSEARCHPARAMS;
}

static js_object *urlparams_this(js_ctx *ctx, js_value value)
{
    if (value.type != JS_OBJECT ||
        value.u.obj->cls != JS_CLASS_URLSEARCHPARAMS ||
        !value.u.obj->params_list) {
        js_throw_error(ctx, JS_ERR_TYPE, "invalid URLSearchParams receiver");
        return 0;
    }
    return value.u.obj;
}

static int array_like_length(js_ctx *ctx, js_object *object,
                             unsigned long *length)
{
    js_value value;
    uint32_t converted;

    if (object->cls == JS_CLASS_ARRAY) {
        *length = object->elen;
        return JS_OK;
    }
    if (js_obj_get(ctx, object, ctx->s_length, js_object_value(object),
                   &value) != JS_OK ||
        js_to_uint32(ctx, value, &converted) != JS_OK)
        return JS_THROW;
    *length = converted;
    return JS_OK;
}

static int array_like_get(js_ctx *ctx, js_object *object,
                          unsigned long index, js_value *out)
{
    char digits[24], reverse[24];
    unsigned long n = index;
    int used = 0, count = 0;
    js_string *key;

    if (object->cls == JS_CLASS_ARRAY && index < object->elen) {
        *out = object->elems[index];
        return JS_OK;
    }
    if (!n)
        digits[used++] = '0';
    while (n) {
        reverse[count++] = (char)('0' + n % 10);
        n /= 10;
    }
    while (count)
        digits[used++] = reverse[--count];
    key = js_str_intern(ctx, digits, (unsigned long)used);
    if (!key)
        return JS_THROW;
    return js_obj_get(ctx, object, key, js_object_value(object), out);
}

static int params_push(js_ctx *ctx, js_object *params,
                       js_value key, js_value value)
{
    js_object *list = params->params_list;

    if (list->elen / 2 >= URLPARAMS_MAX_PAIRS)
        return js_throw_error(ctx, JS_ERR_RANGE,
                              "URLSearchParams pair limit exceeded");
    if (js_array_push(ctx, list, key) != JS_OK)
        return JS_THROW;
    if (js_array_push(ctx, list, value) != JS_OK) {
        list->elen--;
        return JS_THROW;
    }
    params->byte_length++;
    return JS_OK;
}

static int params_push_converted(js_ctx *ctx, js_object *params,
                                 js_value key, js_value value)
{
    js_value key_string, value_string;

    if (js_to_string(ctx, key, &key_string) != JS_OK ||
        js_to_string(ctx, value, &value_string) != JS_OK)
        return JS_THROW;
    return params_push(ctx, params, key_string, value_string);
}

static int hex_value(int c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static int params_decode_component(js_ctx *ctx, const char *source,
                                   unsigned long length, js_value *out)
{
    js_sbuf decoded;
    unsigned long i;

    js_sb_init(&decoded, ctx);
    for (i = 0; i < length; i++) {
        char c = source[i];

        if (c == '+') {
            if (js_sb_putc(&decoded, ' ') != JS_OK)
                goto fail;
        } else if (c == '%' && i + 2 < length) {
            int hi = hex_value((unsigned char)source[i + 1]);
            int lo = hex_value((unsigned char)source[i + 2]);

            if (hi >= 0 && lo >= 0) {
                if (js_sb_putc(&decoded, (char)(hi * 16 + lo)) != JS_OK)
                    goto fail;
                i += 2;
            } else if (js_sb_putc(&decoded, c) != JS_OK) {
                goto fail;
            }
        } else if (js_sb_putc(&decoded, c) != JS_OK) {
            goto fail;
        }
    }
    return js_sb_finish(&decoded, out);

fail:
    js_sb_free(&decoded);
    return JS_THROW;
}

static int params_parse(js_ctx *ctx, js_object *params, js_string *query)
{
    unsigned long at = query->len && query->data[0] == '?' ? 1 : 0;

    if (query->len > URLPARAMS_MAX_INPUT)
        return js_throw_error(ctx, JS_ERR_RANGE,
                              "URLSearchParams input is too large");
    while (at < query->len) {
        unsigned long end = at, equal;
        js_value key, value;

        while (end < query->len && query->data[end] != '&')
            end++;
        if (end == at) {
            at = end + (end < query->len);
            continue;
        }
        equal = at;
        while (equal < end && query->data[equal] != '=')
            equal++;
        if (params_decode_component(ctx, query->data + at,
                                    equal - at, &key) != JS_OK ||
            params_decode_component(ctx,
                                    equal < end ? query->data + equal + 1 :
                                                  query->data + end,
                                    equal < end ? end - equal - 1 : 0,
                                    &value) != JS_OK ||
            params_push(ctx, params, key, value) != JS_OK)
            return JS_THROW;
        at = end + (end < query->len);
    }
    return JS_OK;
}

static int urlparams_ctor(js_ctx *ctx, js_value this_value, int argc,
                          js_value *argv, js_value *out)
{
    js_object *params;
    js_value input = web_arg(argc, argv, 0);

    (void)this_value;
    if (!ctx->new_target)
        return js_throw_error(ctx, JS_ERR_TYPE,
                              "URLSearchParams must be constructed with new");
    params = js_obj_alloc(ctx, JS_CLASS_URLSEARCHPARAMS,
                          ctx->proto[P_URLSEARCHPARAMS]);
    if (!params)
        return JS_THROW;
    params->params_list = js_new_array(ctx);
    if (!params->params_list)
        return JS_THROW;
    *out = js_object_value(params);
    if (input.type == JS_UNDEFINED)
        return JS_OK;
    if (input.type == JS_OBJECT &&
        input.u.obj->cls == JS_CLASS_URLSEARCHPARAMS) {
        js_object *source = input.u.obj->params_list;
        uint32_t i;

        for (i = 0; i + 1 < source->elen; i += 2)
            if (params_push(ctx, params, source->elems[i],
                            source->elems[i + 1]) != JS_OK)
                return JS_THROW;
        return JS_OK;
    }
    if (input.type == JS_OBJECT && input.u.obj->cls == JS_CLASS_ARRAY) {
        unsigned long i;

        for (i = 0; i < input.u.obj->elen; i++) {
            js_value pair = input.u.obj->elems[i];
            unsigned long pair_length;
            js_value key, value;

            if (pair.type != JS_OBJECT ||
                array_like_length(ctx, pair.u.obj, &pair_length) != JS_OK)
                return js_throw_error(ctx, JS_ERR_TYPE,
                                      "URLSearchParams pair is not a sequence");
            if (pair_length != 2)
                return js_throw_error(ctx, JS_ERR_TYPE,
                                      "URLSearchParams pair needs two items");
            if (array_like_get(ctx, pair.u.obj, 0, &key) != JS_OK ||
                array_like_get(ctx, pair.u.obj, 1, &value) != JS_OK ||
                params_push_converted(ctx, params, key, value) != JS_OK)
                return JS_THROW;
        }
        return JS_OK;
    }
    if (input.type == JS_OBJECT) {
        uint32_t i;

        for (i = 0; i < input.u.obj->nprops; i++) {
            js_prop *property = &input.u.obj->props[i];
            js_value value;

            if ((property->flags & JS_P_DEAD) ||
                !(property->flags & JS_P_ENUM))
                continue;
            if (js_obj_get(ctx, input.u.obj, property->key, input,
                           &value) != JS_OK ||
                params_push_converted(ctx, params,
                    js_string_value(property->key), value) != JS_OK)
                return JS_THROW;
        }
        return JS_OK;
    }
    {
        js_value string;

        if (js_to_string(ctx, input, &string) != JS_OK)
            return JS_THROW;
        return params_parse(ctx, params, string.u.str);
    }
}

static int params_name_value(js_ctx *ctx, int argc, js_value *argv,
                             js_value *name, js_value *value,
                             int need_value)
{
    if (js_to_string(ctx, web_arg(argc, argv, 0), name) != JS_OK)
        return JS_THROW;
    if (need_value &&
        js_to_string(ctx, web_arg(argc, argv, 1), value) != JS_OK)
        return JS_THROW;
    return JS_OK;
}

static int params_require_args(js_ctx *ctx, int argc, int required,
                               const char *method)
{
    if (argc >= required)
        return JS_OK;
    return js_throw_error(ctx, JS_ERR_TYPE,
                          "URLSearchParams.%s needs %d argument%s",
                          method, required, required == 1 ? "" : "s");
}

static int urlparams_size(js_ctx *ctx, js_value this_value, int argc,
                          js_value *argv, js_value *out)
{
    js_object *params = urlparams_this(ctx, this_value);

    (void)argc;
    (void)argv;
    if (!params)
        return JS_THROW;
    *out = js_number((double)(params->params_list->elen / 2));
    return JS_OK;
}

static int urlparams_append(js_ctx *ctx, js_value this_value, int argc,
                            js_value *argv, js_value *out)
{
    js_object *params = urlparams_this(ctx, this_value);
    js_value name, value;

    if (!params ||
        params_require_args(ctx, argc, 2, "append") != JS_OK ||
        params_name_value(ctx, argc, argv, &name, &value, 1) != JS_OK ||
        params_push(ctx, params, name, value) != JS_OK)
        return JS_THROW;
    *out = js_undefined();
    return JS_OK;
}

static int urlparams_delete(js_ctx *ctx, js_value this_value, int argc,
                            js_value *argv, js_value *out)
{
    js_object *params = urlparams_this(ctx, this_value);
    js_object *list;
    js_value name, value = js_undefined();
    uint32_t i = 0;

    if (!params ||
        params_require_args(ctx, argc, 1, "delete") != JS_OK ||
        params_name_value(ctx, argc, argv, &name, &value,
                          argc > 1) != JS_OK)
        return JS_THROW;
    list = params->params_list;
    while (i + 1 < list->elen) {
        if (js_str_eq(list->elems[i].u.str, name.u.str) &&
            (argc < 2 || js_str_eq(list->elems[i + 1].u.str,
                                   value.u.str))) {
            memmove(list->elems + i, list->elems + i + 2,
                    (unsigned long)(list->elen - i - 2) *
                    sizeof(*list->elems));
            list->elen -= 2;
            params->byte_length++;
        } else {
            i += 2;
        }
    }
    *out = js_undefined();
    return JS_OK;
}

static int urlparams_get(js_ctx *ctx, js_value this_value, int argc,
                         js_value *argv, js_value *out)
{
    js_object *params = urlparams_this(ctx, this_value);
    js_value name, unused;
    uint32_t i;

    if (!params ||
        params_require_args(ctx, argc, 1, "get") != JS_OK ||
        params_name_value(ctx, argc, argv, &name, &unused, 0) != JS_OK)
        return JS_THROW;
    for (i = 0; i + 1 < params->params_list->elen; i += 2)
        if (js_str_eq(params->params_list->elems[i].u.str, name.u.str)) {
            *out = params->params_list->elems[i + 1];
            return JS_OK;
        }
    *out = js_null();
    return JS_OK;
}

static int urlparams_get_all(js_ctx *ctx, js_value this_value, int argc,
                             js_value *argv, js_value *out)
{
    js_object *params = urlparams_this(ctx, this_value);
    js_object *values;
    js_value name, unused;
    uint32_t i;

    if (!params ||
        params_require_args(ctx, argc, 1, "getAll") != JS_OK ||
        params_name_value(ctx, argc, argv, &name, &unused, 0) != JS_OK)
        return JS_THROW;
    values = js_new_array(ctx);
    if (!values)
        return JS_THROW;
    for (i = 0; i + 1 < params->params_list->elen; i += 2)
        if (js_str_eq(params->params_list->elems[i].u.str, name.u.str) &&
            js_array_push(ctx, values,
                          params->params_list->elems[i + 1]) != JS_OK)
            return JS_THROW;
    *out = js_object_value(values);
    return JS_OK;
}

static int urlparams_has(js_ctx *ctx, js_value this_value, int argc,
                         js_value *argv, js_value *out)
{
    js_object *params = urlparams_this(ctx, this_value);
    js_value name, value = js_undefined();
    uint32_t i;

    if (!params ||
        params_require_args(ctx, argc, 1, "has") != JS_OK ||
        params_name_value(ctx, argc, argv, &name, &value,
                          argc > 1) != JS_OK)
        return JS_THROW;
    for (i = 0; i + 1 < params->params_list->elen; i += 2)
        if (js_str_eq(params->params_list->elems[i].u.str, name.u.str) &&
            (argc < 2 || js_str_eq(params->params_list->elems[i + 1].u.str,
                                   value.u.str))) {
            *out = js_bool(1);
            return JS_OK;
        }
    *out = js_bool(0);
    return JS_OK;
}

static int urlparams_set(js_ctx *ctx, js_value this_value, int argc,
                         js_value *argv, js_value *out)
{
    js_object *params = urlparams_this(ctx, this_value);
    js_object *list;
    js_value name, value;
    uint32_t i = 0;
    int found = 0;

    if (!params ||
        params_require_args(ctx, argc, 2, "set") != JS_OK ||
        params_name_value(ctx, argc, argv, &name, &value, 1) != JS_OK)
        return JS_THROW;
    list = params->params_list;
    while (i + 1 < list->elen) {
        if (!js_str_eq(list->elems[i].u.str, name.u.str)) {
            i += 2;
            continue;
        }
        if (!found) {
            list->elems[i + 1] = value;
            found = 1;
            params->byte_length++;
            i += 2;
        } else {
            memmove(list->elems + i, list->elems + i + 2,
                    (unsigned long)(list->elen - i - 2) *
                    sizeof(*list->elems));
            list->elen -= 2;
            params->byte_length++;
        }
    }
    if (!found && params_push(ctx, params, name, value) != JS_OK)
        return JS_THROW;
    *out = js_undefined();
    return JS_OK;
}

static int string_compare(js_string *a, js_string *b)
{
    unsigned long length = a->len < b->len ? a->len : b->len;
    int result = memcmp(a->data, b->data, length);

    if (result)
        return result;
    return a->len < b->len ? -1 : a->len > b->len;
}

static int urlparams_sort(js_ctx *ctx, js_value this_value, int argc,
                          js_value *argv, js_value *out)
{
    js_object *params = urlparams_this(ctx, this_value);
    js_object *list;
    uint32_t i;

    (void)argc;
    (void)argv;
    if (!params)
        return JS_THROW;
    list = params->params_list;
    for (i = 2; i + 1 < list->elen; i += 2) {
        js_value key = list->elems[i];
        js_value value = list->elems[i + 1];
        uint32_t at = i;

        while (at >= 2 &&
               string_compare(list->elems[at - 2].u.str, key.u.str) > 0) {
            list->elems[at] = list->elems[at - 2];
            list->elems[at + 1] = list->elems[at - 1];
            at -= 2;
        }
        list->elems[at] = key;
        list->elems[at + 1] = value;
    }
    params->byte_length++;
    *out = js_undefined();
    return JS_OK;
}

static int params_encode_component(js_sbuf *output, js_string *string)
{
    static const char hex[] = "0123456789ABCDEF";
    uint32_t i;

    for (i = 0; i < string->len; i++) {
        unsigned int c = (unsigned char)string->data[i];

        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '*' || c == '-' ||
            c == '.' || c == '_') {
            if (js_sb_putc(output, (char)c) != JS_OK)
                return 0;
        } else if (c == ' ') {
            if (js_sb_putc(output, '+') != JS_OK)
                return 0;
        } else {
            char encoded[3] = {
                '%', hex[(c >> 4) & 15], hex[c & 15]
            };

            if (js_sb_put(output, encoded, 3) != JS_OK)
                return 0;
        }
    }
    return 1;
}

static int urlparams_to_string(js_ctx *ctx, js_value this_value, int argc,
                               js_value *argv, js_value *out)
{
    js_object *params = urlparams_this(ctx, this_value);
    js_object *list;
    js_sbuf result;
    uint32_t i;

    (void)argc;
    (void)argv;
    if (!params)
        return JS_THROW;
    list = params->params_list;
    js_sb_init(&result, ctx);
    for (i = 0; i + 1 < list->elen; i += 2) {
        if (i && js_sb_putc(&result, '&') != JS_OK)
            goto fail;
        if (!params_encode_component(&result, list->elems[i].u.str) ||
            js_sb_putc(&result, '=') != JS_OK ||
            !params_encode_component(&result, list->elems[i + 1].u.str))
            goto fail;
        if (result.n > URLPARAMS_MAX_INPUT)
            goto too_large;
    }
    return js_sb_finish(&result, out);

too_large:
    js_sb_free(&result);
    return js_throw_error(ctx, JS_ERR_RANGE,
                          "URLSearchParams output is too large");
fail:
    js_sb_free(&result);
    return JS_THROW;
}

js_value js_urlsearchparams_new(js_ctx *ctx, const char *query,
                                unsigned long length)
{
    js_object *params;
    js_string *source;

    params = js_obj_alloc(ctx, JS_CLASS_URLSEARCHPARAMS,
                          ctx->proto[P_URLSEARCHPARAMS]);
    if (!params)
        return js_undefined();
    params->params_list = js_new_array(ctx);
    if (!params->params_list)
        return js_undefined();
    source = js_str_new(ctx, query ? query : "", query ? length : 0);
    if (!source || params_parse(ctx, params, source) != JS_OK)
        return js_undefined();
    return js_object_value(params);
}

int js_urlsearchparams_replace(js_ctx *ctx, js_value value,
                               const char *query, unsigned long length)
{
    js_object *params = urlparams_this(ctx, value);
    js_value replacement;

    if (!params)
        return JS_THROW;
    replacement = js_urlsearchparams_new(ctx, query, length);
    if (replacement.type != JS_OBJECT)
        return JS_THROW;
    params->params_list = replacement.u.obj->params_list;
    params->byte_length++;
    return JS_OK;
}

int js_urlsearchparams_string(js_ctx *ctx, js_value params, js_value *out)
{
    return urlparams_to_string(ctx, params, 0, 0, out);
}

static int urlparams_for_each(js_ctx *ctx, js_value this_value, int argc,
                              js_value *argv, js_value *out)
{
    js_object *params = urlparams_this(ctx, this_value);
    js_value callback = web_arg(argc, argv, 0);
    js_value receiver = web_arg(argc, argv, 1);
    uint32_t at = 0, turns = 0;

    if (!params)
        return JS_THROW;
    if (!js_is_function(callback))
        return js_throw_error(ctx, JS_ERR_TYPE,
                              "URLSearchParams.forEach needs a function");
    while (at + 1 < params->params_list->elen &&
           turns++ < URLPARAMS_MAX_PAIRS) {
        js_value args[3], ignored;

        args[0] = params->params_list->elems[at + 1];
        args[1] = params->params_list->elems[at];
        args[2] = this_value;
        if (js_call(ctx, callback, receiver, 3, args, &ignored) != JS_OK)
            return JS_THROW;
        at += 2;
    }
    *out = js_undefined();
    return JS_OK;
}

static int urlparams_iter_self(js_ctx *ctx, js_value this_value, int argc,
                               js_value *argv, js_value *out)
{
    (void)ctx;
    (void)argc;
    (void)argv;
    *out = this_value;
    return JS_OK;
}

static int urlparams_iterator_next(js_ctx *ctx, js_value this_value, int argc,
                                   js_value *argv, js_value *out)
{
    js_value source_val, index_val, kind_val;
    js_object *params, *result;
    unsigned long index;
    int kind;

    (void)argc;
    (void)argv;
    if (this_value.type != JS_OBJECT ||
        js_get(ctx, this_value, "__params", &source_val) != JS_OK ||
        js_get(ctx, this_value, "__index", &index_val) != JS_OK ||
        js_get(ctx, this_value, "__kind", &kind_val) != JS_OK ||
        !(params = urlparams_this(ctx, source_val)) ||
        index_val.type != JS_NUMBER || kind_val.type != JS_NUMBER)
        return js_throw_error(ctx, JS_ERR_TYPE,
                              "invalid URLSearchParams iterator");
    index = index_val.u.num < 0 ? 0 : (unsigned long)index_val.u.num;
    kind = (int)kind_val.u.num;
    result = js_new_object(ctx);
    if (!result)
        return JS_THROW;
    if (index * 2 + 1 >= params->params_list->elen) {
        if (js_define_enum(ctx, result, "value", js_undefined()) != JS_OK ||
            js_define_enum(ctx, result, "done", js_bool(1)) != JS_OK)
            return JS_THROW;
    } else {
        js_value key = params->params_list->elems[index * 2];
        js_value val = params->params_list->elems[index * 2 + 1];
        js_value item;

        if (kind == 0) {
            item = key;
        } else if (kind == 1) {
            item = val;
        } else {
            js_object *pair = js_new_array(ctx);
            if (!pair ||
                js_array_push(ctx, pair, key) != JS_OK ||
                js_array_push(ctx, pair, val) != JS_OK)
                return JS_THROW;
            item = js_object_value(pair);
        }
        if (js_define_enum(ctx, result, "value", item) != JS_OK ||
            js_define_enum(ctx, result, "done", js_bool(0)) != JS_OK ||
            js_set(ctx, this_value, "__index",
                   js_number((double)(index + 1))) != JS_OK)
            return JS_THROW;
    }
    *out = js_object_value(result);
    return JS_OK;
}

static int urlparams_iterator(js_ctx *ctx, js_value this_value,
                              js_value *out, int kind)
{
    js_object *params = urlparams_this(ctx, this_value);
    js_object *iterator;

    if (!params)
        return JS_THROW;
    iterator = js_new_object(ctx);
    if (!iterator ||
        js_define(ctx, iterator, "__params", this_value) != JS_OK ||
        js_define(ctx, iterator, "__index", js_number(0)) != JS_OK ||
        js_define(ctx, iterator, "__kind", js_number(kind)) != JS_OK ||
        js_define_native(ctx, iterator, "next",
                         urlparams_iterator_next, 0) != JS_OK)
        return JS_THROW;
    if (ctx->sym_iterator.type == JS_SYMBOL) {
        js_object *self_fn = js_new_native(ctx, urlparams_iter_self,
                                           "[Symbol.iterator]", 0);
        if (!self_fn ||
            js_define_symbol_prop(ctx, iterator, ctx->sym_iterator,
                                  js_object_value(self_fn),
                                  JS_P_HIDDEN) != JS_OK)
            return JS_THROW;
    }
    *out = js_object_value(iterator);
    return JS_OK;
}

static int urlparams_keys(js_ctx *ctx, js_value this_value, int argc,
                          js_value *argv, js_value *out)
{
    (void)argc;
    (void)argv;
    return urlparams_iterator(ctx, this_value, out, 0);
}

static int urlparams_values(js_ctx *ctx, js_value this_value, int argc,
                            js_value *argv, js_value *out)
{
    (void)argc;
    (void)argv;
    return urlparams_iterator(ctx, this_value, out, 1);
}

static int urlparams_entries(js_ctx *ctx, js_value this_value, int argc,
                             js_value *argv, js_value *out)
{
    (void)argc;
    (void)argv;
    return urlparams_iterator(ctx, this_value, out, 2);
}

/* ================================================================== */
/* WHATWG URL                                                          */
/* ================================================================== */

typedef struct js_url_state {
    js_string *scheme;
    js_string *username;
    js_string *password;
    js_string *host;
    int        port;
    uint8_t    has_authority;
    uint8_t    is_ipv6;
    uint8_t    opaque_path;
    uint8_t    has_query;
    uint8_t    has_fragment;
    js_string *path;
    js_string *query;
    js_string *fragment;
    js_value   search_params;
    uint32_t   params_version;
} js_url_state;

enum url_enc_set {
    URL_ENC_FRAG = 0,
    URL_ENC_QUERY,
    URL_ENC_SPECIAL_QUERY,
    URL_ENC_PATH,
    URL_ENC_USERINFO
};

static int url_is_alpha(int c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static int url_is_digit(int c)
{
    return c >= '0' && c <= '9';
}

static int url_lower_char(int c)
{
    return (c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c;
}

static int url_scheme_eq(js_string *scheme, const char *lit)
{
    unsigned long n = strlen(lit);
    return scheme && scheme->len == n && memcmp(scheme->data, lit, n) == 0;
}

static int url_default_port(js_string *scheme)
{
    if (url_scheme_eq(scheme, "http") || url_scheme_eq(scheme, "ws"))
        return 80;
    if (url_scheme_eq(scheme, "https") || url_scheme_eq(scheme, "wss"))
        return 443;
    if (url_scheme_eq(scheme, "ftp"))
        return 21;
    return -1;
}

static int url_is_http_like(js_string *scheme)
{
    return url_scheme_eq(scheme, "http") || url_scheme_eq(scheme, "https") ||
           url_scheme_eq(scheme, "ws") || url_scheme_eq(scheme, "wss") ||
           url_scheme_eq(scheme, "ftp");
}

static int url_is_special(js_string *scheme)
{
    return url_is_http_like(scheme) || url_scheme_eq(scheme, "file");
}

static int url_needs_encode(unsigned char c, int set)
{
    if (c <= 0x1F || c >= 0x7F)
        return 1;
    if (c == ' ' || c == '"' || c == '<' || c == '>')
        return 1;
    if (set == URL_ENC_FRAG)
        return c == '`';
    if (c == '#')
        return 1;
    if (set == URL_ENC_SPECIAL_QUERY && c == '\'')
        return 1;
    if (set == URL_ENC_QUERY || set == URL_ENC_SPECIAL_QUERY)
        return 0;
    if (c == '?' || c == '`' || c == '{' || c == '}')
        return 1;
    if (set == URL_ENC_PATH)
        return 0;
    return c == '/' || c == ':' || c == ';' || c == '=' ||
           c == '@' || (c >= '[' && c <= '^') || c == '|';
}

static js_string *url_pct_encode(js_ctx *ctx, const char *in,
                                 unsigned long len, int set)
{
    static const char hex[] = "0123456789ABCDEF";
    js_sbuf b;
    unsigned long i;
    js_value sv;

    js_sb_init(&b, ctx);
    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char)in[i];

        if (c == '%' && i + 2 < len &&
            hex_value((unsigned char)in[i + 1]) >= 0 &&
            hex_value((unsigned char)in[i + 2]) >= 0) {
            if (js_sb_put(&b, in + i, 3) != JS_OK)
                goto fail;
            i += 2;
            continue;
        }
        if (url_needs_encode(c, set)) {
            char enc[3];
            enc[0] = '%';
            enc[1] = hex[(c >> 4) & 15];
            enc[2] = hex[c & 15];
            if (js_sb_put(&b, enc, 3) != JS_OK)
                goto fail;
        } else if (js_sb_putc(&b, (char)c) != JS_OK) {
            goto fail;
        }
    }
    if (js_sb_finish(&b, &sv) != JS_OK)
        return 0;
    return sv.u.str;
fail:
    js_sb_free(&b);
    return 0;
}

static js_string *url_preprocess(js_ctx *ctx, const char *s, unsigned long len)
{
    js_sbuf b;
    unsigned long a = 0, e = len, i;
    js_value sv;

    while (a < e && (unsigned char)s[a] <= 0x20)
        a++;
    while (e > a && (unsigned char)s[e - 1] <= 0x20)
        e--;
    js_sb_init(&b, ctx);
    for (i = a; i < e; i++) {
        char c = s[i];
        if (c == '\t' || c == '\n' || c == '\r')
            continue;
        if (js_sb_putc(&b, c) != JS_OK) {
            js_sb_free(&b);
            return 0;
        }
    }
    if (js_sb_finish(&b, &sv) != JS_OK)
        return 0;
    return sv.u.str;
}

static int url_valid_scheme(const char *s, unsigned long len)
{
    unsigned long i;

    if (!len || !url_is_alpha((unsigned char)s[0]))
        return 0;
    for (i = 1; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (!url_is_alpha(c) && !url_is_digit(c) &&
            c != '+' && c != '-' && c != '.')
            return 0;
    }
    return 1;
}

static js_string *url_lower_str(js_ctx *ctx, const char *s, unsigned long len)
{
    js_sbuf b;
    unsigned long i;
    js_value sv;

    js_sb_init(&b, ctx);
    for (i = 0; i < len; i++) {
        if (js_sb_putc(&b, (char)url_lower_char((unsigned char)s[i])) != JS_OK) {
            js_sb_free(&b);
            return 0;
        }
    }
    if (js_sb_finish(&b, &sv) != JS_OK)
        return 0;
    return sv.u.str;
}

static int url_is_single_dot(const char *s, unsigned long n)
{
    if (n == 1 && s[0] == '.')
        return 1;
    if (n == 3 && s[0] == '%' && s[1] == '2' &&
        (s[2] == 'e' || s[2] == 'E'))
        return 1;
    return 0;
}

static int url_is_double_dot(const char *s, unsigned long n)
{
    if (n == 2 && s[0] == '.' && s[1] == '.')
        return 1;
    if (n == 4 && s[0] == '.' && s[1] == '%' && s[2] == '2' &&
        (s[3] == 'e' || s[3] == 'E'))
        return 1;
    if (n == 4 && s[0] == '%' && s[1] == '2' &&
        (s[2] == 'e' || s[2] == 'E') && s[3] == '.')
        return 1;
    if (n == 6 && s[0] == '%' && s[1] == '2' &&
        (s[2] == 'e' || s[2] == 'E') &&
        s[3] == '%' && s[4] == '2' && (s[5] == 'e' || s[5] == 'E'))
        return 1;
    return 0;
}

static js_string *url_normalize_path(js_ctx *ctx, const char *raw,
                                     unsigned long len, int is_special,
                                     int has_authority)
{
    js_object *segs = js_new_array(ctx);
    js_sbuf norm, joined;
    unsigned long i = 0, pos = 0;
    int leading_slash = 0;
    js_value tmp, sv;

    if (!segs)
        return 0;
    js_sb_init(&norm, ctx);
    for (i = 0; i < len; i++) {
        char c = (is_special && raw[i] == '\\') ? '/' : raw[i];
        if (js_sb_putc(&norm, c) != JS_OK) {
            js_sb_free(&norm);
            return 0;
        }
    }
    if (js_sb_finish(&norm, &tmp) != JS_OK)
        return 0;

    raw = tmp.u.str->data;
    len = tmp.u.str->len;
    if (len > 0 && raw[0] == '/') {
        leading_slash = 1;
        pos = 1;
    } else if (is_special || (has_authority && len > 0)) {
        leading_slash = 1;
    }

    if (len > pos || !leading_slash) {
        while (pos <= len) {
            unsigned long end = pos;
            int last;
            while (end < len && raw[end] != '/')
                end++;
            last = (end == len);
            if (url_is_double_dot(raw + pos, end - pos)) {
                if (segs->elen > 0)
                    segs->elen--;
                if (last && js_array_push(ctx, segs,
                                          js_string_value(ctx->s_empty)) != JS_OK)
                    return 0;
            } else if (url_is_single_dot(raw + pos, end - pos)) {
                if (last && js_array_push(ctx, segs,
                                          js_string_value(ctx->s_empty)) != JS_OK)
                    return 0;
            } else {
                js_value seg = js_mkstring(ctx, raw + pos, end - pos);
                if (seg.type != JS_STRING ||
                    js_array_push(ctx, segs, seg) != JS_OK)
                    return 0;
            }
            if (last)
                break;
            pos = end + 1;
        }
    }

    js_sb_init(&joined, ctx);
    if (leading_slash && js_sb_putc(&joined, '/') != JS_OK)
        goto fail;
    for (i = 0; i < segs->elen; i++) {
        js_string *sg = segs->elems[i].u.str;
        if (i && js_sb_putc(&joined, '/') != JS_OK)
            goto fail;
        if (js_sb_put(&joined, sg->data, sg->len) != JS_OK)
            goto fail;
    }
    if (js_sb_finish(&joined, &sv) != JS_OK)
        return 0;
    return url_pct_encode(ctx, sv.u.str->data, sv.u.str->len, URL_ENC_PATH);
fail:
    js_sb_free(&joined);
    return 0;
}

static int url_is_forbidden_host_char(unsigned char c, int is_special)
{
    if (c <= 0x20 || c == 0x7F || c == '#' || c == '/' || c == ':' ||
        c == '<' || c == '>' || c == '?' || c == '@' || c == '[' ||
        c == '\\' || c == ']' || c == '^' || c == '|')
        return 1;
    if (is_special && c == '%')
        return 1;
    return 0;
}

static int url_parse_hostport(js_ctx *ctx, js_string *scheme,
                              const char *hp, unsigned long len,
                              int allow_port, js_string **out_host,
                              uint8_t *out_ipv6, int *out_port)
{
    int special = url_is_special(scheme);
    int def_port = url_default_port(scheme);

    *out_ipv6 = 0;
    *out_port = -1;
    if (len > 0 && hp[0] == '[') {
        unsigned long close = 1, i;
        int colons = 0;
        while (close < len && hp[close] != ']')
            close++;
        if (close >= len || close == 1)
            return 0;
        for (i = 1; i < close; i++) {
            unsigned char c = (unsigned char)hp[i];
            if (c == ':')
                colons++;
            else if (hex_value(c) < 0 && c != '.')
                return 0;
        }
        if (!colons)
            return 0;
        *out_host = url_lower_str(ctx, hp + 1, close - 1);
        *out_ipv6 = 1;
        if (close + 1 < len) {
            if (!allow_port || hp[close + 1] != ':')
                return 0;
            hp += close + 2;
            len -= close + 2;
        } else {
            len = 0;
        }
    } else {
        unsigned long colon = 0, i;
        while (colon < len && hp[colon] != ':')
            colon++;
        if (colon < len && !allow_port)
            return 0;
        for (i = 0; i < colon; i++) {
            if (url_is_forbidden_host_char((unsigned char)hp[i], special))
                return 0;
        }
        if (colon == 0 && url_is_http_like(scheme))
            return 0;
        *out_host = url_lower_str(ctx, hp, colon);
        if (colon < len) {
            hp += colon + 1;
            len -= colon + 1;
        } else {
            len = 0;
        }
    }
    if (!*out_host)
        return 0;
    if (len > 0) {
        unsigned long i, pval = 0;
        if (url_scheme_eq(scheme, "file"))
            return 0;
        for (i = 0; i < len; i++) {
            if (!url_is_digit((unsigned char)hp[i]))
                return 0;
            pval = pval * 10 + (unsigned long)(hp[i] - '0');
            if (pval > 65535)
                return 0;
        }
        *out_port = ((int)pval == def_port) ? -1 : (int)pval;
    }
    return 1;
}

static int url_parse_authority(js_ctx *ctx, const char *a, unsigned long len,
                               js_url_state *st)
{
    unsigned long i, at = len;
    const char *hp;
    unsigned long hplen;

    for (i = 0; i < len; i++)
        if (a[i] == '@')
            at = i;
    if (at < len) {
        unsigned long colon = 0;
        if (url_scheme_eq(st->scheme, "file") && at > 0)
            return 0;
        while (colon < at && a[colon] != ':')
            colon++;
        st->username = url_pct_encode(ctx, a, colon, URL_ENC_USERINFO);
        st->password = colon < at
            ? url_pct_encode(ctx, a + colon + 1, at - colon - 1,
                             URL_ENC_USERINFO)
            : ctx->s_empty;
        if (!st->username || !st->password)
            return 0;
        hp = a + at + 1;
        hplen = len - at - 1;
    } else {
        st->username = ctx->s_empty;
        st->password = ctx->s_empty;
        hp = a;
        hplen = len;
    }
    if (!url_parse_hostport(ctx, st->scheme, hp, hplen, 1,
                            &st->host, &st->is_ipv6, &st->port))
        return 0;
    st->has_authority = 1;
    return 1;
}

static int url_parse_pqf(js_ctx *ctx, const char *s, unsigned long len,
                         int is_special, int has_authority,
                         js_string **out_path, uint8_t *out_hq,
                         js_string **out_q, uint8_t *out_hf,
                         js_string **out_f)
{
    unsigned long q = 0, h;

    while (q < len && s[q] != '?' && s[q] != '#')
        q++;
    *out_path = url_normalize_path(ctx, s, q, is_special, has_authority);
    if (!*out_path)
        return 0;
    *out_hq = 0;
    *out_q = ctx->s_empty;
    *out_hf = 0;
    *out_f = ctx->s_empty;
    if (q < len && s[q] == '?') {
        unsigned long qs = q + 1;
        h = qs;
        while (h < len && s[h] != '#')
            h++;
        *out_q = url_pct_encode(ctx, s + qs, h - qs,
                                is_special ? URL_ENC_SPECIAL_QUERY : URL_ENC_QUERY);
        if (!*out_q)
            return 0;
        *out_hq = 1;
        q = h;
    }
    if (q < len && s[q] == '#') {
        *out_f = url_pct_encode(ctx, s + q + 1, len - q - 1, URL_ENC_FRAG);
        if (!*out_f)
            return 0;
        *out_hf = 1;
    }
    return 1;
}

static void url_init_empty_state(js_ctx *ctx, js_url_state *st)
{
    memset(st, 0, sizeof(*st));
    st->scheme = ctx->s_empty;
    st->username = ctx->s_empty;
    st->password = ctx->s_empty;
    st->host = ctx->s_empty;
    st->path = ctx->s_empty;
    st->query = ctx->s_empty;
    st->fragment = ctx->s_empty;
    st->port = -1;
}

static int url_is_slash(char c, int special)
{
    return c == '/' || (special && c == '\\');
}

static int url_parse_core(js_ctx *ctx, js_string *input,
                          const js_url_state *base, js_url_state *out)
{
    const char *s = input->data;
    unsigned long len = input->len, pos = 0, colon = 0;
    int has_scheme = 0, special;
    js_string *scheme = 0;

    url_init_empty_state(ctx, out);
    if (len > 0 && url_is_alpha((unsigned char)s[0])) {
        while (colon < len && (url_is_alpha((unsigned char)s[colon]) ||
                               url_is_digit((unsigned char)s[colon]) ||
                               s[colon] == '+' || s[colon] == '-' ||
                               s[colon] == '.'))
            colon++;
        if (colon < len && s[colon] == ':')
            has_scheme = 1;
    }

    if (has_scheme) {
        scheme = url_lower_str(ctx, s, colon);
        if (!scheme) return 0;
        special = url_is_special(scheme);
        pos = colon + 1;
        if (base && special && js_str_eq(scheme, base->scheme) &&
            !(pos + 1 < len && url_is_slash(s[pos], 1) &&
              url_is_slash(s[pos + 1], 1))) {
            /* Special scheme relative to matching base without // */
            has_scheme = 0;
        } else {
            out->scheme = scheme;
            if (special) {
                while (pos < len && url_is_slash(s[pos], 1))
                    pos++;
                if (url_scheme_eq(scheme, "file")) {
                    unsigned long ae = pos;
                    if (colon + 2 < len && url_is_slash(s[colon + 1], 1) &&
                        url_is_slash(s[colon + 2], 1)) {
                        while (ae < len && !url_is_slash(s[ae], 1) &&
                               s[ae] != '?' && s[ae] != '#')
                            ae++;
                        if (ae > pos) {
                            if (!url_parse_authority(ctx, s + pos, ae - pos, out))
                                return 0;
                            pos = ae;
                        } else {
                            out->has_authority = 1;
                        }
                    } else {
                        out->has_authority = 1;
                        pos = colon + 1;
                    }
                    return url_parse_pqf(ctx, s + pos, len - pos, 1, 1,
                                         &out->path, &out->has_query,
                                         &out->query, &out->has_fragment,
                                         &out->fragment);
                }
                {
                    unsigned long ae = pos;
                    while (ae < len && !url_is_slash(s[ae], 1) &&
                           s[ae] != '?' && s[ae] != '#')
                        ae++;
                    if (!url_parse_authority(ctx, s + pos, ae - pos, out))
                        return 0;
                    pos = ae;
                    return url_parse_pqf(ctx, s + pos, len - pos, 1, 1,
                                         &out->path, &out->has_query,
                                         &out->query, &out->has_fragment,
                                         &out->fragment);
                }
            }
            /* Non-special scheme */
            if (pos + 1 < len && s[pos] == '/' && s[pos + 1] == '/') {
                unsigned long ae = pos + 2;
                while (ae < len && s[ae] != '/' && s[ae] != '?' && s[ae] != '#')
                    ae++;
                if (!url_parse_authority(ctx, s + pos + 2, ae - (pos + 2), out))
                    return 0;
                pos = ae;
                return url_parse_pqf(ctx, s + pos, len - pos, 0, 1,
                                     &out->path, &out->has_query,
                                     &out->query, &out->has_fragment,
                                     &out->fragment);
            }
            if (pos < len && s[pos] == '/') {
                return url_parse_pqf(ctx, s + pos, len - pos, 0, 0,
                                     &out->path, &out->has_query,
                                     &out->query, &out->has_fragment,
                                     &out->fragment);
            }
            /* Opaque path (e.g. mailto:, data:, blob:) */
            {
                unsigned long q = pos, h;
                out->opaque_path = 1;
                while (q < len && s[q] != '?' && s[q] != '#')
                    q++;
                out->path = url_pct_encode(ctx, s + pos, q - pos, URL_ENC_FRAG);
                if (!out->path) return 0;
                if (q < len && s[q] == '?') {
                    h = q + 1;
                    while (h < len && s[h] != '#')
                        h++;
                    out->query = url_pct_encode(ctx, s + q + 1, h - q - 1,
                                                URL_ENC_QUERY);
                    if (!out->query) return 0;
                    out->has_query = 1;
                    q = h;
                }
                if (q < len && s[q] == '#') {
                    out->fragment = url_pct_encode(ctx, s + q + 1,
                                                   len - q - 1, URL_ENC_FRAG);
                    if (!out->fragment) return 0;
                    out->has_fragment = 1;
                }
                return 1;
            }
        }
    }

    if (!base)
        return 0;

    out->scheme = base->scheme;
    out->username = base->username;
    out->password = base->password;
    out->host = base->host;
    out->port = base->port;
    out->has_authority = base->has_authority;
    out->is_ipv6 = base->is_ipv6;
    out->opaque_path = base->opaque_path;
    special = url_is_special(out->scheme);

    if (base->opaque_path) {
        if (pos >= len || s[pos] != '#')
            return 0;
        out->path = base->path;
        out->has_query = base->has_query;
        out->query = base->query;
        out->fragment = url_pct_encode(ctx, s + pos + 1, len - pos - 1,
                                       URL_ENC_FRAG);
        if (!out->fragment) return 0;
        out->has_fragment = 1;
        return 1;
    }

    if (pos == len) {
        out->path = base->path;
        out->has_query = base->has_query;
        out->query = base->query;
        return 1;
    }
    if (s[pos] == '#') {
        out->path = base->path;
        out->has_query = base->has_query;
        out->query = base->query;
        out->fragment = url_pct_encode(ctx, s + pos + 1, len - pos - 1,
                                       URL_ENC_FRAG);
        if (!out->fragment) return 0;
        out->has_fragment = 1;
        return 1;
    }
    if (s[pos] == '?') {
        unsigned long h = pos + 1;
        out->path = base->path;
        while (h < len && s[h] != '#')
            h++;
        out->query = url_pct_encode(ctx, s + pos + 1, h - pos - 1,
                                    special ? URL_ENC_SPECIAL_QUERY : URL_ENC_QUERY);
        if (!out->query) return 0;
        out->has_query = 1;
        if (h < len && s[h] == '#') {
            out->fragment = url_pct_encode(ctx, s + h + 1, len - h - 1,
                                           URL_ENC_FRAG);
            if (!out->fragment) return 0;
            out->has_fragment = 1;
        }
        return 1;
    }
    if (pos + 1 < len && url_is_slash(s[pos], special) &&
        url_is_slash(s[pos + 1], special)) {
        unsigned long ae = pos + 2;
        out->username = ctx->s_empty;
        out->password = ctx->s_empty;
        out->host = ctx->s_empty;
        out->port = -1;
        out->has_authority = 0;
        out->is_ipv6 = 0;
        while (ae < len && !url_is_slash(s[ae], special) &&
               s[ae] != '?' && s[ae] != '#')
            ae++;
        if (!url_parse_authority(ctx, s + pos + 2, ae - (pos + 2), out))
            return 0;
        return url_parse_pqf(ctx, s + ae, len - ae, special, 1,
                             &out->path, &out->has_query, &out->query,
                             &out->has_fragment, &out->fragment);
    }
    if (url_is_slash(s[pos], special)) {
        return url_parse_pqf(ctx, s + pos, len - pos, special,
                             out->has_authority,
                             &out->path, &out->has_query, &out->query,
                             &out->has_fragment, &out->fragment);
    }
    {
        /* Relative path: merge with base directory */
        unsigned long keep = 0, i, q = pos, h;
        js_sbuf merged;
        js_value mv;

        for (i = 0; i < base->path->len; i++)
            if (base->path->data[i] == '/')
                keep = i + 1;
        while (q < len && s[q] != '?' && s[q] != '#')
            q++;
        js_sb_init(&merged, ctx);
        if (keep) {
            if (js_sb_put(&merged, base->path->data, keep) != JS_OK)
                goto merge_fail;
        } else if (out->has_authority) {
            if (js_sb_putc(&merged, '/') != JS_OK)
                goto merge_fail;
        }
        if (js_sb_put(&merged, s + pos, q - pos) != JS_OK)
            goto merge_fail;
        if (js_sb_finish(&merged, &mv) != JS_OK)
            return 0;
        out->path = url_normalize_path(ctx, mv.u.str->data, mv.u.str->len,
                                       special, out->has_authority);
        if (!out->path)
            return 0;
        if (q < len && s[q] == '?') {
            h = q + 1;
            while (h < len && s[h] != '#')
                h++;
            out->query = url_pct_encode(ctx, s + q + 1, h - q - 1,
                                        special ? URL_ENC_SPECIAL_QUERY : URL_ENC_QUERY);
            if (!out->query) return 0;
            out->has_query = 1;
            q = h;
        }
        if (q < len && s[q] == '#') {
            out->fragment = url_pct_encode(ctx, s + q + 1, len - q - 1,
                                           URL_ENC_FRAG);
            if (!out->fragment) return 0;
            out->has_fragment = 1;
        }
        return 1;
merge_fail:
        js_sb_free(&merged);
        return 0;
    }
}

static int url_sb_put_uint(js_sbuf *b, unsigned int v)
{
    char tmp[12];
    int n = 0;

    if (v == 0)
        tmp[n++] = '0';
    while (v > 0 && n < (int)sizeof(tmp)) {
        tmp[n++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (n > 0) {
        if (js_sb_putc(b, tmp[--n]) != JS_OK)
            return JS_THROW;
    }
    return JS_OK;
}

static int url_sync_params(js_ctx *ctx, js_url_state *st)
{
    js_value ser;

    if (!js_is_urlsearchparams(st->search_params))
        return JS_OK;
    if (st->search_params.u.obj->byte_length == st->params_version)
        return JS_OK;
    if (js_urlsearchparams_string(ctx, st->search_params, &ser) != JS_OK)
        return JS_THROW;
    st->query = ser.u.str;
    st->has_query = ser.u.str->len > 0;
    st->params_version = st->search_params.u.obj->byte_length;
    return JS_OK;
}

static js_url_state *url_this(js_ctx *ctx, js_value v)
{
    if (v.type != JS_OBJECT || v.u.obj->cls != JS_CLASS_URL || !v.u.obj->host) {
        js_throw_error(ctx, JS_ERR_TYPE, "invalid URL receiver");
        return 0;
    }
    return (js_url_state *)v.u.obj->host;
}

static int url_parse_args(js_ctx *ctx, int argc, js_value *argv,
                          js_url_state *out, int *out_ok)
{
    js_value us, bs;
    js_string *u_pre, *b_pre;
    js_url_state base_st;

    *out_ok = 0;
    if (js_to_string(ctx, argv[0], &us) != JS_OK)
        return JS_THROW;
    u_pre = url_preprocess(ctx, us.u.str->data, us.u.str->len);
    if (!u_pre)
        return JS_THROW;
    if (argc > 1 && argv[1].type != JS_UNDEFINED) {
        if (js_to_string(ctx, argv[1], &bs) != JS_OK)
            return JS_THROW;
        b_pre = url_preprocess(ctx, bs.u.str->data, bs.u.str->len);
        if (!b_pre)
            return JS_THROW;
        if (!url_parse_core(ctx, b_pre, 0, &base_st))
            return JS_OK;
        if (!url_parse_core(ctx, u_pre, &base_st, out))
            return JS_OK;
    } else {
        if (!url_parse_core(ctx, u_pre, 0, out))
            return JS_OK;
    }
    *out_ok = 1;
    return JS_OK;
}

static int url_ctor(js_ctx *ctx, js_value this_value, int argc,
                    js_value *argv, js_value *out)
{
    js_url_state *st;
    js_object *obj;
    int ok = 0;
    (void)this_value;

    if (!ctx->new_target)
        return js_throw_error(ctx, JS_ERR_TYPE,
                              "URL must be constructed with new");
    if (argc < 1)
        return js_throw_error(ctx, JS_ERR_TYPE, "URL requires an input");
    st = (js_url_state *)js_alloc(ctx, sizeof(js_url_state));
    if (!st)
        return JS_THROW;
    if (url_parse_args(ctx, argc, argv, st, &ok) != JS_OK)
        return JS_THROW;
    if (!ok)
        return js_throw_error(ctx, JS_ERR_TYPE, "Invalid URL");
    st->search_params = js_urlsearchparams_new(ctx, st->query->data,
                                               st->query->len);
    if (!js_is_object(st->search_params))
        return JS_THROW;
    st->params_version = st->search_params.u.obj->byte_length;
    obj = js_obj_alloc(ctx, JS_CLASS_URL, ctx->proto[P_URL]);
    if (!obj)
        return JS_THROW;
    obj->host = st;
    *out = js_object_value(obj);
    return JS_OK;
}

static int url_can_parse(js_ctx *ctx, js_value this_value, int argc,
                         js_value *argv, js_value *out)
{
    js_url_state tmp;
    int ok = 0;
    (void)this_value;

    if (argc < 1)
        return js_throw_error(ctx, JS_ERR_TYPE,
                              "URL.canParse requires at least 1 argument");
    if (url_parse_args(ctx, argc, argv, &tmp, &ok) != JS_OK)
        return JS_THROW;
    *out = js_bool(ok);
    return JS_OK;
}

static int url_serialize(js_ctx *ctx, js_url_state *st, js_value *out)
{
    js_sbuf b;

    if (url_sync_params(ctx, st) != JS_OK)
        return JS_THROW;
    js_sb_init(&b, ctx);
    if (js_sb_put(&b, st->scheme->data, st->scheme->len) != JS_OK ||
        js_sb_putc(&b, ':') != JS_OK)
        goto fail;
    if (st->has_authority) {
        if (js_sb_puts(&b, "//") != JS_OK)
            goto fail;
        if (st->username->len || st->password->len) {
            if (js_sb_put(&b, st->username->data, st->username->len) != JS_OK)
                goto fail;
            if (st->password->len) {
                if (js_sb_putc(&b, ':') != JS_OK ||
                    js_sb_put(&b, st->password->data, st->password->len) != JS_OK)
                    goto fail;
            }
            if (js_sb_putc(&b, '@') != JS_OK)
                goto fail;
        }
        if (st->is_ipv6 && js_sb_putc(&b, '[') != JS_OK)
            goto fail;
        if (js_sb_put(&b, st->host->data, st->host->len) != JS_OK)
            goto fail;
        if (st->is_ipv6 && js_sb_putc(&b, ']') != JS_OK)
            goto fail;
        if (st->port >= 0) {
            if (js_sb_putc(&b, ':') != JS_OK ||
                url_sb_put_uint(&b, (unsigned int)st->port) != JS_OK)
                goto fail;
        }
    }
    if (js_sb_put(&b, st->path->data, st->path->len) != JS_OK)
        goto fail;
    if (st->has_query) {
        if (js_sb_putc(&b, '?') != JS_OK ||
            js_sb_put(&b, st->query->data, st->query->len) != JS_OK)
            goto fail;
    }
    if (st->has_fragment) {
        if (js_sb_putc(&b, '#') != JS_OK ||
            js_sb_put(&b, st->fragment->data, st->fragment->len) != JS_OK)
            goto fail;
    }
    return js_sb_finish(&b, out);
fail:
    js_sb_free(&b);
    return JS_THROW;
}

static int url_href_get(js_ctx *ctx, js_value this_value, int argc,
                        js_value *argv, js_value *out)
{
    js_url_state *st = url_this(ctx, this_value);
    (void)argc; (void)argv;
    if (!st) return JS_THROW;
    return url_serialize(ctx, st, out);
}

static int url_href_set(js_ctx *ctx, js_value this_value, int argc,
                        js_value *argv, js_value *out)
{
    js_url_state *st = url_this(ctx, this_value);
    js_value sv;
    js_string *pre;
    js_url_state next;

    if (!st) return JS_THROW;
    if (js_to_string(ctx, web_arg(argc, argv, 0), &sv) != JS_OK)
        return JS_THROW;
    pre = url_preprocess(ctx, sv.u.str->data, sv.u.str->len);
    if (!pre) return JS_THROW;
    if (!url_parse_core(ctx, pre, 0, &next))
        return js_throw_error(ctx, JS_ERR_TYPE, "Invalid URL");
    next.search_params = st->search_params;
    if (js_urlsearchparams_replace(ctx, st->search_params,
                                   next.query->data, next.query->len) != JS_OK)
        return JS_THROW;
    next.params_version = st->search_params.u.obj->byte_length;
    *st = next;
    *out = js_undefined();
    return JS_OK;
}

static int url_format_origin(js_ctx *ctx, const js_url_state *st, js_value *out)
{
    js_sbuf b;

    if (url_scheme_eq(st->scheme, "blob")) {
        js_url_state inner;
        if (url_parse_core(ctx, st->path, 0, &inner) &&
            url_is_http_like(inner.scheme))
            return url_format_origin(ctx, &inner, out);
        return web_return_string(ctx, "null", 4, out);
    }
    if (!url_is_http_like(st->scheme))
        return web_return_string(ctx, "null", 4, out);
    js_sb_init(&b, ctx);
    if (js_sb_put(&b, st->scheme->data, st->scheme->len) != JS_OK ||
        js_sb_puts(&b, "://") != JS_OK)
        goto fail;
    if (st->is_ipv6 && js_sb_putc(&b, '[') != JS_OK)
        goto fail;
    if (js_sb_put(&b, st->host->data, st->host->len) != JS_OK)
        goto fail;
    if (st->is_ipv6 && js_sb_putc(&b, ']') != JS_OK)
        goto fail;
    if (st->port >= 0 && st->port != url_default_port(st->scheme)) {
        if (js_sb_putc(&b, ':') != JS_OK ||
            url_sb_put_uint(&b, (unsigned int)st->port) != JS_OK)
            goto fail;
    }
    return js_sb_finish(&b, out);
fail:
    js_sb_free(&b);
    return JS_THROW;
}

static int url_origin_get(js_ctx *ctx, js_value this_value, int argc,
                          js_value *argv, js_value *out)
{
    js_url_state *st = url_this(ctx, this_value);
    (void)argc; (void)argv;
    if (!st) return JS_THROW;
    return url_format_origin(ctx, st, out);
}

static int url_protocol_get(js_ctx *ctx, js_value this_value, int argc,
                            js_value *argv, js_value *out)
{
    js_url_state *st = url_this(ctx, this_value);
    js_sbuf b;
    (void)argc; (void)argv;

    if (!st) return JS_THROW;
    js_sb_init(&b, ctx);
    if (js_sb_put(&b, st->scheme->data, st->scheme->len) != JS_OK ||
        js_sb_putc(&b, ':') != JS_OK) {
        js_sb_free(&b);
        return JS_THROW;
    }
    return js_sb_finish(&b, out);
}

static int url_protocol_set(js_ctx *ctx, js_value this_value, int argc,
                            js_value *argv, js_value *out)
{
    js_url_state *st = url_this(ctx, this_value);
    js_value sv;
    unsigned long n = 0;
    js_string *ns;

    if (!st) return JS_THROW;
    if (js_to_string(ctx, web_arg(argc, argv, 0), &sv) != JS_OK)
        return JS_THROW;
    while (n < sv.u.str->len && sv.u.str->data[n] != ':')
        n++;
    if (url_valid_scheme(sv.u.str->data, n)) {
        ns = url_lower_str(ctx, sv.u.str->data, n);
        if (!ns) return JS_THROW;
        if (url_is_special(st->scheme) == url_is_special(ns) &&
            url_scheme_eq(st->scheme, "file") == url_scheme_eq(ns, "file")) {
            st->scheme = ns;
            if (st->port >= 0 && st->port == url_default_port(ns))
                st->port = -1;
        }
    }
    *out = js_undefined();
    return JS_OK;
}

static int url_username_get(js_ctx *ctx, js_value this_value, int argc,
                            js_value *argv, js_value *out)
{
    js_url_state *st = url_this(ctx, this_value);
    (void)argc; (void)argv;
    if (!st) return JS_THROW;
    *out = js_string_value(st->username);
    return JS_OK;
}

static int url_username_set(js_ctx *ctx, js_value this_value, int argc,
                            js_value *argv, js_value *out)
{
    js_url_state *st = url_this(ctx, this_value);
    js_value sv;

    if (!st) return JS_THROW;
    if (js_to_string(ctx, web_arg(argc, argv, 0), &sv) != JS_OK)
        return JS_THROW;
    if (st->has_authority && !st->opaque_path && st->host->len > 0 &&
        !url_scheme_eq(st->scheme, "file")) {
        js_string *enc = url_pct_encode(ctx, sv.u.str->data, sv.u.str->len,
                                        URL_ENC_USERINFO);
        if (!enc) return JS_THROW;
        st->username = enc;
    }
    *out = js_undefined();
    return JS_OK;
}

static int url_password_get(js_ctx *ctx, js_value this_value, int argc,
                            js_value *argv, js_value *out)
{
    js_url_state *st = url_this(ctx, this_value);
    (void)argc; (void)argv;
    if (!st) return JS_THROW;
    *out = js_string_value(st->password);
    return JS_OK;
}

static int url_password_set(js_ctx *ctx, js_value this_value, int argc,
                            js_value *argv, js_value *out)
{
    js_url_state *st = url_this(ctx, this_value);
    js_value sv;

    if (!st) return JS_THROW;
    if (js_to_string(ctx, web_arg(argc, argv, 0), &sv) != JS_OK)
        return JS_THROW;
    if (st->has_authority && !st->opaque_path && st->host->len > 0 &&
        !url_scheme_eq(st->scheme, "file")) {
        js_string *enc = url_pct_encode(ctx, sv.u.str->data, sv.u.str->len,
                                        URL_ENC_USERINFO);
        if (!enc) return JS_THROW;
        st->password = enc;
    }
    *out = js_undefined();
    return JS_OK;
}

static int url_format_hostname(js_ctx *ctx, const js_url_state *st,
                               int include_port, js_value *out)
{
    js_sbuf b;

    if (!st->has_authority || st->host->len == 0)
        return web_return_string(ctx, "", 0, out);
    js_sb_init(&b, ctx);
    if (st->is_ipv6 && js_sb_putc(&b, '[') != JS_OK)
        goto fail;
    if (js_sb_put(&b, st->host->data, st->host->len) != JS_OK)
        goto fail;
    if (st->is_ipv6 && js_sb_putc(&b, ']') != JS_OK)
        goto fail;
    if (include_port && st->port >= 0) {
        if (js_sb_putc(&b, ':') != JS_OK ||
            url_sb_put_uint(&b, (unsigned int)st->port) != JS_OK)
            goto fail;
    }
    return js_sb_finish(&b, out);
fail:
    js_sb_free(&b);
    return JS_THROW;
}

static int url_host_get(js_ctx *ctx, js_value this_value, int argc,
                        js_value *argv, js_value *out)
{
    js_url_state *st = url_this(ctx, this_value);
    (void)argc; (void)argv;
    if (!st) return JS_THROW;
    return url_format_hostname(ctx, st, 1, out);
}

static int url_hostname_get(js_ctx *ctx, js_value this_value, int argc,
                            js_value *argv, js_value *out)
{
    js_url_state *st = url_this(ctx, this_value);
    (void)argc; (void)argv;
    if (!st) return JS_THROW;
    return url_format_hostname(ctx, st, 0, out);
}

static int url_host_set_impl(js_ctx *ctx, js_value this_value, int argc,
                             js_value *argv, js_value *out, int allow_port)
{
    js_url_state *st = url_this(ctx, this_value);
    js_value sv;
    js_string *nh = 0;
    uint8_t nip6 = 0;
    int np = -1;

    if (!st) return JS_THROW;
    if (js_to_string(ctx, web_arg(argc, argv, 0), &sv) != JS_OK)
        return JS_THROW;
    if (!st->opaque_path &&
        url_parse_hostport(ctx, st->scheme, sv.u.str->data, sv.u.str->len,
                           allow_port, &nh, &nip6, &np)) {
        st->host = nh;
        st->is_ipv6 = nip6;
        if (allow_port)
            st->port = np;
        st->has_authority = 1;
    }
    *out = js_undefined();
    return JS_OK;
}

static int url_host_set(js_ctx *ctx, js_value this_value, int argc,
                        js_value *argv, js_value *out)
{ return url_host_set_impl(ctx, this_value, argc, argv, out, 1); }

static int url_hostname_set(js_ctx *ctx, js_value this_value, int argc,
                            js_value *argv, js_value *out)
{ return url_host_set_impl(ctx, this_value, argc, argv, out, 0); }

static int url_port_get(js_ctx *ctx, js_value this_value, int argc,
                        js_value *argv, js_value *out)
{
    js_url_state *st = url_this(ctx, this_value);
    js_sbuf b;
    (void)argc; (void)argv;

    if (!st) return JS_THROW;
    if (st->port < 0)
        return web_return_string(ctx, "", 0, out);
    js_sb_init(&b, ctx);
    if (url_sb_put_uint(&b, (unsigned int)st->port) != JS_OK) {
        js_sb_free(&b);
        return JS_THROW;
    }
    return js_sb_finish(&b, out);
}

static int url_port_set(js_ctx *ctx, js_value this_value, int argc,
                        js_value *argv, js_value *out)
{
    js_url_state *st = url_this(ctx, this_value);
    js_value sv;
    const char *s;
    unsigned long len, i, pv = 0;

    if (!st) return JS_THROW;
    if (js_to_string(ctx, web_arg(argc, argv, 0), &sv) != JS_OK)
        return JS_THROW;
    if (st->has_authority && !st->opaque_path && st->host->len > 0 &&
        !url_scheme_eq(st->scheme, "file")) {
        s = sv.u.str->data;
        len = sv.u.str->len;
        if (len == 0) {
            st->port = -1;
        } else {
            for (i = 0; i < len && url_is_digit((unsigned char)s[i]); i++) {
                pv = pv * 10 + (unsigned long)(s[i] - '0');
                if (pv > 65535)
                    break;
            }
            if (i == len && pv <= 65535) {
                int dp = url_default_port(st->scheme);
                st->port = ((int)pv == dp) ? -1 : (int)pv;
            }
        }
    }
    *out = js_undefined();
    return JS_OK;
}

static int url_pathname_get(js_ctx *ctx, js_value this_value, int argc,
                            js_value *argv, js_value *out)
{
    js_url_state *st = url_this(ctx, this_value);
    (void)argc; (void)argv;
    if (!st) return JS_THROW;
    *out = js_string_value(st->path);
    return JS_OK;
}

static int url_pathname_set(js_ctx *ctx, js_value this_value, int argc,
                            js_value *argv, js_value *out)
{
    js_url_state *st = url_this(ctx, this_value);
    js_value sv;

    if (!st) return JS_THROW;
    if (js_to_string(ctx, web_arg(argc, argv, 0), &sv) != JS_OK)
        return JS_THROW;
    if (!st->opaque_path) {
        js_string *np = url_normalize_path(ctx, sv.u.str->data, sv.u.str->len,
                                           url_is_special(st->scheme),
                                           st->has_authority);
        if (!np) return JS_THROW;
        st->path = np;
    }
    *out = js_undefined();
    return JS_OK;
}

static int url_search_get(js_ctx *ctx, js_value this_value, int argc,
                          js_value *argv, js_value *out)
{
    js_url_state *st = url_this(ctx, this_value);
    js_sbuf b;
    (void)argc; (void)argv;

    if (!st) return JS_THROW;
    if (url_sync_params(ctx, st) != JS_OK)
        return JS_THROW;
    if (!st->has_query || st->query->len == 0)
        return web_return_string(ctx, "", 0, out);
    js_sb_init(&b, ctx);
    if (js_sb_putc(&b, '?') != JS_OK ||
        js_sb_put(&b, st->query->data, st->query->len) != JS_OK) {
        js_sb_free(&b);
        return JS_THROW;
    }
    return js_sb_finish(&b, out);
}

static int url_search_set(js_ctx *ctx, js_value this_value, int argc,
                          js_value *argv, js_value *out)
{
    js_url_state *st = url_this(ctx, this_value);
    js_value sv;
    const char *s;
    unsigned long len;

    if (!st) return JS_THROW;
    if (js_to_string(ctx, web_arg(argc, argv, 0), &sv) != JS_OK)
        return JS_THROW;
    s = sv.u.str->data;
    len = sv.u.str->len;
    if (len == 0) {
        st->query = ctx->s_empty;
        st->has_query = 0;
    } else {
        if (s[0] == '?') {
            s++;
            len--;
        }
        st->query = url_pct_encode(ctx, s, len,
                                   url_is_special(st->scheme)
                                       ? URL_ENC_SPECIAL_QUERY : URL_ENC_QUERY);
        if (!st->query) return JS_THROW;
        st->has_query = st->query->len > 0;
    }
    if (js_urlsearchparams_replace(ctx, st->search_params,
                                   st->query->data, st->query->len) != JS_OK)
        return JS_THROW;
    st->params_version = st->search_params.u.obj->byte_length;
    *out = js_undefined();
    return JS_OK;
}

static int url_search_params_get(js_ctx *ctx, js_value this_value, int argc,
                                 js_value *argv, js_value *out)
{
    js_url_state *st = url_this(ctx, this_value);
    (void)argc; (void)argv;
    if (!st) return JS_THROW;
    *out = st->search_params;
    return JS_OK;
}

static int url_hash_get(js_ctx *ctx, js_value this_value, int argc,
                        js_value *argv, js_value *out)
{
    js_url_state *st = url_this(ctx, this_value);
    js_sbuf b;
    (void)argc; (void)argv;

    if (!st) return JS_THROW;
    if (!st->has_fragment || st->fragment->len == 0)
        return web_return_string(ctx, "", 0, out);
    js_sb_init(&b, ctx);
    if (js_sb_putc(&b, '#') != JS_OK ||
        js_sb_put(&b, st->fragment->data, st->fragment->len) != JS_OK) {
        js_sb_free(&b);
        return JS_THROW;
    }
    return js_sb_finish(&b, out);
}

static int url_hash_set(js_ctx *ctx, js_value this_value, int argc,
                        js_value *argv, js_value *out)
{
    js_url_state *st = url_this(ctx, this_value);
    js_value sv;
    const char *s;
    unsigned long len;

    if (!st) return JS_THROW;
    if (js_to_string(ctx, web_arg(argc, argv, 0), &sv) != JS_OK)
        return JS_THROW;
    s = sv.u.str->data;
    len = sv.u.str->len;
    if (len == 0) {
        st->fragment = ctx->s_empty;
        st->has_fragment = 0;
    } else {
        if (s[0] == '#') {
            s++;
            len--;
        }
        st->fragment = url_pct_encode(ctx, s, len, URL_ENC_FRAG);
        if (!st->fragment) return JS_THROW;
        st->has_fragment = st->fragment->len > 0;
    }
    *out = js_undefined();
    return JS_OK;
}

static int url_to_string(js_ctx *ctx, js_value this_value, int argc,
                         js_value *argv, js_value *out)
{
    return url_href_get(ctx, this_value, argc, argv, out);
}

/* ================================================================== */
/* Registration                                                        */
/* ================================================================== */

int js_init_webapis(js_ctx *ctx)
{
    js_object *proto, *url_ctor_obj;

    proto = ctx->proto[P_TEXTENCODER];
    if (js_define_accessor(ctx, proto, "encoding",
                           text_encoder_encoding, 0, 0) != JS_OK ||
        !web_method(ctx, proto, "encode", text_encoder_encode, 1) ||
        !web_method(ctx, proto, "encodeInto",
                    text_encoder_encode_into, 2) ||
        !web_constructor(ctx, text_encoder_ctor, "TextEncoder", 0, proto))
        return 0;

    proto = ctx->proto[P_TEXTDECODER];
    if (js_define_accessor(ctx, proto, "encoding",
                           text_decoder_encoding, 0, 0) != JS_OK ||
        js_define_accessor(ctx, proto, "fatal",
                           text_decoder_fatal, 0, 0) != JS_OK ||
        js_define_accessor(ctx, proto, "ignoreBOM",
                           text_decoder_ignore_bom, 0, 0) != JS_OK ||
        !web_method(ctx, proto, "decode", text_decoder_decode, 1) ||
        !web_constructor(ctx, text_decoder_ctor, "TextDecoder", 0, proto))
        return 0;

    proto = ctx->proto[P_URLSEARCHPARAMS];
    if (js_define_accessor(ctx, proto, "size",
                           urlparams_size, 0, 0) != JS_OK ||
        !web_method(ctx, proto, "append", urlparams_append, 2) ||
        !web_method(ctx, proto, "delete", urlparams_delete, 1) ||
        !web_method(ctx, proto, "get", urlparams_get, 1) ||
        !web_method(ctx, proto, "getAll", urlparams_get_all, 1) ||
        !web_method(ctx, proto, "has", urlparams_has, 1) ||
        !web_method(ctx, proto, "set", urlparams_set, 2) ||
        !web_method(ctx, proto, "sort", urlparams_sort, 0) ||
        !web_method(ctx, proto, "forEach", urlparams_for_each, 1) ||
        !web_method(ctx, proto, "keys", urlparams_keys, 0) ||
        !web_method(ctx, proto, "values", urlparams_values, 0) ||
        !web_method(ctx, proto, "toString", urlparams_to_string, 0) ||
        !web_constructor(ctx, urlparams_ctor, "URLSearchParams", 1, proto))
        return 0;
    {
        js_object *entries_fn = js_new_native(ctx, urlparams_entries,
                                              "entries", 0);
        if (!entries_fn ||
            js_define(ctx, proto, "entries",
                      js_object_value(entries_fn)) != JS_OK ||
            (ctx->sym_iterator.type == JS_SYMBOL &&
             js_define_symbol_prop(ctx, proto, ctx->sym_iterator,
                                   js_object_value(entries_fn),
                                   JS_P_HIDDEN) != JS_OK))
            return 0;
    }

    proto = ctx->proto[P_URL];
    if (js_define_accessor(ctx, proto, "href",
                           url_href_get, url_href_set, 0) != JS_OK ||
        js_define_accessor(ctx, proto, "origin",
                           url_origin_get, 0, 0) != JS_OK ||
        js_define_accessor(ctx, proto, "protocol",
                           url_protocol_get, url_protocol_set, 0) != JS_OK ||
        js_define_accessor(ctx, proto, "username",
                           url_username_get, url_username_set, 0) != JS_OK ||
        js_define_accessor(ctx, proto, "password",
                           url_password_get, url_password_set, 0) != JS_OK ||
        js_define_accessor(ctx, proto, "host",
                           url_host_get, url_host_set, 0) != JS_OK ||
        js_define_accessor(ctx, proto, "hostname",
                           url_hostname_get, url_hostname_set, 0) != JS_OK ||
        js_define_accessor(ctx, proto, "port",
                           url_port_get, url_port_set, 0) != JS_OK ||
        js_define_accessor(ctx, proto, "pathname",
                           url_pathname_get, url_pathname_set, 0) != JS_OK ||
        js_define_accessor(ctx, proto, "search",
                           url_search_get, url_search_set, 0) != JS_OK ||
        js_define_accessor(ctx, proto, "searchParams",
                           url_search_params_get, 0, 0) != JS_OK ||
        js_define_accessor(ctx, proto, "hash",
                           url_hash_get, url_hash_set, 0) != JS_OK ||
        !web_method(ctx, proto, "toString", url_to_string, 0) ||
        !web_method(ctx, proto, "toJSON", url_to_string, 0))
        return 0;
    url_ctor_obj = web_constructor(ctx, url_ctor, "URL", 1, proto);
    if (!url_ctor_obj ||
        !web_method(ctx, url_ctor_obj, "canParse", url_can_parse, 1))
        return 0;
    return 1;
}
