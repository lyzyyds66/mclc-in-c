#include "inflate.h"
#include "util.h"

#include <stdlib.h>
#include <string.h>

#define MCLC_MAX_INFLATE_OUTPUT ((size_t)512 * 1024 * 1024)

typedef struct {
    const unsigned char *input;
    size_t input_size;
    size_t input_pos;
    uint32_t bit_buffer;
    int bit_count;
    unsigned char *output;
    size_t output_size;
    size_t output_cap;
    size_t expected;
    int failed;
    char **error;
} InflateState;

typedef struct {
    unsigned short count[16];
    unsigned short symbol[288];
} Huffman;

static int inf_fail(InflateState *s, const char *message)
{
    if (!s->failed) {
        s->failed = 1;
        set_error(s->error, "DEFLATE 解压失败: %s", message);
    }
    return -1;
}

static int need_bits(InflateState *s, int count)
{
    while (s->bit_count < count) {
        if (s->input_pos >= s->input_size) {
            return inf_fail(s, "数据提前结束");
        }
        s->bit_buffer |= (uint32_t)s->input[s->input_pos++] << s->bit_count;
        s->bit_count += 8;
    }
    return 0;
}

static int get_bits(InflateState *s, int count)
{
    uint32_t value;

    if (count == 0) {
        return 0;
    }
    if (need_bits(s, count) != 0) {
        return -1;
    }
    value = s->bit_buffer & ((1u << count) - 1u);
    s->bit_buffer >>= count;
    s->bit_count -= count;
    return (int)value;
}

static void align_byte(InflateState *s)
{
    /*
     * Discard the remaining bits of the current byte. Whole bytes that were
     * prefetched into the bit buffer must be handed back to the input cursor;
     * stored blocks read directly from it.
     */
    size_t whole = (size_t)s->bit_count / 8;
    if (whole <= s->input_pos) {
        s->input_pos -= whole;
    } else {
        s->input_pos = 0;
    }
    s->bit_buffer = 0;
    s->bit_count = 0;
}

static int output_reserve(InflateState *s, size_t add)
{
    size_t need;
    size_t cap;

    if (add > MCLC_MAX_INFLATE_OUTPUT - s->output_size) {
        return inf_fail(s, "输出超过安全限制");
    }
    need = s->output_size + add;
    if (need <= s->output_cap) {
        return 0;
    }
    if (s->expected) {
        return inf_fail(s, "输出超过声明的解压大小");
    }
    cap = s->output_cap ? s->output_cap : 65536;
    while (cap < need) {
        if (cap > MCLC_MAX_INFLATE_OUTPUT / 2) {
            cap = MCLC_MAX_INFLATE_OUTPUT;
            break;
        }
        cap *= 2;
    }
    s->output = (unsigned char *)xrealloc(s->output, cap);
    s->output_cap = cap;
    return 0;
}

static int output_put(InflateState *s, unsigned char byte)
{
    if (s->output_size >= s->output_cap) {
        if (output_reserve(s, 1) != 0) {
            return -1;
        }
    }
    s->output[s->output_size++] = byte;
    return 0;
}

static int build_huffman(InflateState *s, Huffman *h,
                         const unsigned char *lengths, int n)
{
    int count[16];
    int offsets[16];
    int symbols = 0;
    int left = 1;
    int len;
    int symbol;

    memset(h, 0, sizeof(*h));
    memset(count, 0, sizeof(count));
    for (symbol = 0; symbol < n; symbol++) {
        if (lengths[symbol] > 15) {
            return inf_fail(s, "Huffman 码长无效");
        }
        count[lengths[symbol]]++;
    }
    for (len = 1; len <= 15; len++) {
        left <<= 1;
        left -= count[len];
        if (left < 0) {
            return inf_fail(s, "Huffman 码过度订阅");
        }
    }
    for (len = 0; len <= 15; len++) {
        h->count[len] = (unsigned short)count[len];
        if (len) {
            symbols += count[len];
        }
    }
    if (symbols == 0) {
        return 0;
    }
    offsets[1] = 0;
    for (len = 1; len < 15; len++) {
        offsets[len + 1] = offsets[len] + count[len];
    }
    for (symbol = 0; symbol < n; symbol++) {
        if (lengths[symbol]) {
            h->symbol[offsets[lengths[symbol]]++] = (unsigned short)symbol;
        }
    }
    return 0;
}

static int decode_symbol(InflateState *s, const Huffman *h)
{
    int code = 0;
    int first = 0;
    int index = 0;
    int len;

    for (len = 1; len <= 15; len++) {
        int bit = get_bits(s, 1);
        int count;
        if (bit < 0) {
            return -1;
        }
        code |= bit;
        count = h->count[len];
        if (code - first < count) {
            return h->symbol[index + (code - first)];
        }
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return inf_fail(s, "无效的 Huffman 编码");
}

static int inflate_codes(InflateState *s, const Huffman *lit,
                         const Huffman *dist)
{
    static const unsigned short length_base[29] = {
        3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27,
        31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258
    };
    static const unsigned char length_extra[29] = {
        0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
        2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0
    };
    static const unsigned short distance_base[30] = {
        1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129,
        193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097,
        6145, 8193, 12289, 16385, 24577
    };
    static const unsigned char distance_extra[30] = {
        0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6,
        6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13
    };

    for (;;) {
        int symbol = decode_symbol(s, lit);
        if (symbol < 0) {
            return -1;
        }
        if (symbol < 256) {
            if (output_put(s, (unsigned char)symbol) != 0) {
                return -1;
            }
        } else if (symbol == 256) {
            return 0;
        } else {
            int code = symbol - 257;
            int length;
            int dsym;
            int distance;
            int extra;
            int i;

            if (code < 0 || code >= 29) {
                return inf_fail(s, "无效的长度代码");
            }
            length = length_base[code];
            extra = length_extra[code];
            if (extra) {
                int bits = get_bits(s, extra);
                if (bits < 0) {
                    return -1;
                }
                length += bits;
            }
            dsym = decode_symbol(s, dist);
            if (dsym < 0) {
                return -1;
            }
            if (dsym >= 30) {
                return inf_fail(s, "无效的距离代码");
            }
            distance = distance_base[dsym];
            extra = distance_extra[dsym];
            if (extra) {
                int bits = get_bits(s, extra);
                if (bits < 0) {
                    return -1;
                }
                distance += bits;
            }
            if ((size_t)distance > s->output_size) {
                return inf_fail(s, "距离超出已解压数据");
            }
            if (output_reserve(s, (size_t)length) != 0) {
                return -1;
            }
            for (i = 0; i < length; i++) {
                s->output[s->output_size] =
                    s->output[s->output_size - (size_t)distance];
                s->output_size++;
            }
        }
    }
}

static int inflate_fixed(InflateState *s)
{
    unsigned char lit_lengths[288];
    unsigned char dist_lengths[32];
    Huffman lit;
    Huffman dist;
    int i;

    for (i = 0; i < 144; i++) lit_lengths[i] = 8;
    for (; i < 256; i++) lit_lengths[i] = 9;
    for (; i < 280; i++) lit_lengths[i] = 7;
    for (; i < 288; i++) lit_lengths[i] = 8;
    for (i = 0; i < 32; i++) dist_lengths[i] = 5;
    if (build_huffman(s, &lit, lit_lengths, 288) != 0 ||
        build_huffman(s, &dist, dist_lengths, 32) != 0) {
        return -1;
    }
    return inflate_codes(s, &lit, &dist);
}

static int inflate_dynamic(InflateState *s)
{
    static const unsigned char order[19] = {
        16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
    };
    unsigned char code_lengths[19];
    unsigned char lengths[320];
    Huffman code_tree;
    Huffman lit;
    Huffman dist;
    int hlit;
    int hdist;
    int hclen;
    int total;
    int i;

    hlit = get_bits(s, 5);
    hdist = get_bits(s, 5);
    hclen = get_bits(s, 4);
    if (hlit < 0 || hdist < 0 || hclen < 0) {
        return -1;
    }
    hlit += 257;
    hdist += 1;
    hclen += 4;
    if (hlit > 286 || hdist > 30) {
        return inf_fail(s, "动态 Huffman 头超出范围");
    }

    memset(code_lengths, 0, sizeof(code_lengths));
    for (i = 0; i < hclen; i++) {
        int value = get_bits(s, 3);
        if (value < 0) {
            return -1;
        }
        code_lengths[order[i]] = (unsigned char)value;
    }
    if (build_huffman(s, &code_tree, code_lengths, 19) != 0) {
        return -1;
    }

    total = hlit + hdist;
    memset(lengths, 0, sizeof(lengths));
    i = 0;
    while (i < total) {
        int symbol = decode_symbol(s, &code_tree);
        if (symbol < 0) {
            return -1;
        }
        if (symbol < 16) {
            lengths[i++] = (unsigned char)symbol;
        } else if (symbol == 16) {
            int repeat;
            int value;
            if (i == 0) {
                return inf_fail(s, "重复码没有前置码长");
            }
            repeat = get_bits(s, 2);
            if (repeat < 0) {
                return -1;
            }
            repeat += 3;
            if (i + repeat > total) {
                return inf_fail(s, "码长重复越界");
            }
            value = lengths[i - 1];
            while (repeat--) {
                lengths[i++] = (unsigned char)value;
            }
        } else if (symbol == 17) {
            int repeat = get_bits(s, 3);
            if (repeat < 0) {
                return -1;
            }
            repeat += 3;
            if (i + repeat > total) {
                return inf_fail(s, "零码长重复越界");
            }
            while (repeat--) {
                lengths[i++] = 0;
            }
        } else if (symbol == 18) {
            int repeat = get_bits(s, 7);
            if (repeat < 0) {
                return -1;
            }
            repeat += 11;
            if (i + repeat > total) {
                return inf_fail(s, "长零码重复越界");
            }
            while (repeat--) {
                lengths[i++] = 0;
            }
        } else {
            return inf_fail(s, "无效的码长符号");
        }
    }

    if (build_huffman(s, &lit, lengths, hlit) != 0 ||
        build_huffman(s, &dist, lengths + hlit, hdist) != 0) {
        return -1;
    }
    return inflate_codes(s, &lit, &dist);
}

int mclc_inflate_raw(const unsigned char *input, size_t input_size,
                     size_t expected_size, unsigned char **output,
                     size_t *output_size, char **error)
{
    InflateState s;
    int final = 0;
    int result = -1;

    if (output) {
        *output = NULL;
    }
    if (output_size) {
        *output_size = 0;
    }
    if (!input || input_size == 0) {
        set_error(error, "DEFLATE 输入为空");
        return -1;
    }
    if (expected_size > MCLC_MAX_INFLATE_OUTPUT) {
        set_error(error, "解压后文件过大");
        return -1;
    }

    memset(&s, 0, sizeof(s));
    s.input = input;
    s.input_size = input_size;
    s.expected = expected_size;
    s.error = error;
    if (expected_size) {
        s.output = (unsigned char *)xmalloc(expected_size);
        s.output_cap = expected_size;
    }

    while (!final) {
        int block_type;
        final = get_bits(&s, 1);
        block_type = get_bits(&s, 2);
        if (final < 0 || block_type < 0) {
            goto done;
        }
        if (block_type == 0) {
            unsigned int len;
            unsigned int nlen;
            size_t i;
            align_byte(&s);
            if (s.input_pos + 4 > s.input_size) {
                inf_fail(&s, "存储块头不完整");
                goto done;
            }
            len = (unsigned int)s.input[s.input_pos] |
                  ((unsigned int)s.input[s.input_pos + 1] << 8);
            nlen = (unsigned int)s.input[s.input_pos + 2] |
                   ((unsigned int)s.input[s.input_pos + 3] << 8);
            s.input_pos += 4;
            if ((len ^ 0xFFFFu) != nlen) {
                inf_fail(&s, "存储块长度校验失败");
                goto done;
            }
            if (s.input_pos + len > s.input_size) {
                inf_fail(&s, "存储块数据不完整");
                goto done;
            }
            if (output_reserve(&s, len) != 0) {
                goto done;
            }
            for (i = 0; i < len; i++) {
                s.output[s.output_size++] = s.input[s.input_pos + i];
            }
            s.input_pos += len;
        } else if (block_type == 1) {
            if (inflate_fixed(&s) != 0) {
                goto done;
            }
        } else if (block_type == 2) {
            if (inflate_dynamic(&s) != 0) {
                goto done;
            }
        } else {
            inf_fail(&s, "无效的块类型");
            goto done;
        }
    }

    if (s.expected && s.output_size != s.expected) {
        inf_fail(&s, "解压后大小与声明不符");
        goto done;
    }
    if (!s.output) {
        s.output = (unsigned char *)xmalloc(1);
        s.output_cap = 1;
    }
    result = 0;
    if (output) {
        *output = s.output;
        s.output = NULL;
    }
    if (output_size) {
        *output_size = s.output_size;
    }

done:
    if (result != 0) {
        free(s.output);
    }
    return result;
}
