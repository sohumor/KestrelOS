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
        if (js_is_arraybuffer(argv[0]))
            input = (const unsigned char *)
                js_arraybuffer_data(argv[0], &length);
        else if (js_is_uint8array(argv[0]))
            input = (const unsigned char *)
                js_uint8array_data(argv[0], &length);
        else
            return js_throw_error(ctx, JS_ERR_TYPE,
                                  "TextDecoder.decode needs a byte buffer");
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
            i += 2;
        } else {
            memmove(list->elems + i, list->elems + i + 2,
                    (unsigned long)(list->elen - i - 2) *
                    sizeof(*list->elems));
            list->elen -= 2;
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

/* ================================================================== */
/* Registration                                                        */
/* ================================================================== */

int js_init_webapis(js_ctx *ctx)
{
    js_object *proto;

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
        !web_method(ctx, proto, "toString", urlparams_to_string, 0) ||
        !web_constructor(ctx, urlparams_ctor, "URLSearchParams", 1, proto))
        return 0;
    return 1;
}
