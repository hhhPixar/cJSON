/*
  Copyright (c) 2009-2017 Dave Gamble and cJSON contributors

  Permission is hereby granted, free of charge, to any person obtaining a copy
  of this software and associated documentation files (the "Software"), to deal
  in the Software without restriction, including without limitation the rights
  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
  copies of the Software, and to permit persons to whom the Software is
  furnished to do so, subject to the following conditions:

  The above copyright notice and this permission notice shall be included in
  all copies or substantial portions of the Software.

  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
  THE SOFTWARE.
*/

/* cJSON */
/* JSON parser in C. */
/*
 * =============================================================================
 *  把本文件当 C 教材读（语法 / 指针对照表）
 *
 *  本文件会反复出现这些 C 要点，后面函数里会再对着代码讲一遍：
 *
 *  【编译预处理】
 *    #include     把别的文件粘进来。<> 找系统库，"" 找本目录。
 *    #define A B  预处理器把 A 换成 B，不是真正的变量。
 *    #ifdef / #ifndef / #endif  条件编译：有的宏没定义就不编这段。
 *    #error       编译期直接失败（下面用来检查 .h 和 .c 版本是否一致）。
 *
 *  【类型】
 *    int / double / char / unsigned char / size_t
 *    unsigned char 常用来当“原始字节”，避免 char 有的平台是有符号的。
 *    size_t 是 sizeof、strlen 返回的无符号长度类型。
 *    typedef struct X { ... } X;  给结构体起短名，后面写 X 即可。
 *
 *  【指针 —— 本库的核心】
 *    T *p;            p 里存的是“某块 T 的地址”，不是 T 本身。
 *    *p               解引用：去那个地址上取/改值。
 *    p->field         等价于 (*p).field，结构体指针专用。
 *    NULL             空指针，表示“不指向任何有效对象”。解引用 NULL 会崩溃。
 *    const T *p       不能通过 p 修改 *p（指向的内容只读）。
 *    T * const p      p 本身不能改指向（指针变量只读）。
 *    const T * const p  两者都不能改。本文件参数里大量出现。
 *    p + n            指针加减：跳过 n 个 T，不是加 n 个字节。
 *                     char* 加 1 就是下一个字节；cJSON* 加 1 会跳过整个结构体。
 *    q - p            两个同类型指针相减，得到“中间隔了几个元素”（ptrdiff）。
 *    (T*)expr         强制类型转换。malloc 返回 void*，要转成你真正要用的类型。
 *
 *  【内存】
 *    栈：局部变量，函数返回就没了。parse_buffer buffer = {...} 在栈上。
 *    堆：malloc/calloc 分配，必须自己 free。每个 cJSON 结点都在堆上。
 *    泄漏：malloc 了不 free。
 *    野指针：free 之后还用原来的指针。所以 Delete 里经常 pointer = NULL。
 *
 *  【字符串】
 *    C 字符串 = 一段 char，最后一个字节必须是 '\0'（值为 0）。
 *    "abc" 其实是 'a','b','c','\0'，占 4 字节。strlen("abc") == 3。
 *    char *s 只是指向第一个字符。没有“字符串对象”。
 *
 *  【函数】
 *    static 函数：只在本 .c 文件可见，头文件里没有声明，调用者用不了。
 *    函数指针：void *(*allocate)(size_t);  变量里存的是函数地址，可以换成别的实现。
 *
 *  【其它语法】
 *    if / while / for / do-while / switch-case / goto
 *    本库大量用 goto fail; 做“出错后统一释放”，这是 C 里常见写法，不是坏习惯。
 *    a & b, a | b, a << n   按位与/或/左移，用来把多个标志塞进一个 int。
 *    a && b, a || b, !a     逻辑运算，注意和按位的区别。
 *
 *  建议阅读顺序：
 *    New_Item / Delete / strdup     → 堆、指针、结构体
 *    case_insensitive_strcmp        → *p、p++、C 字符串
 *    parse_buffer 宏                → 指针加减
 *    parse_value / parse_array      → 递归、链表
 *    ensure                         → realloc、动态数组
 *    add_item_to_array / Detach     → 双向链表改指针
 * =============================================================================
 */

/* disable warnings about old C89 functions in MSVC */
#if !defined(_CRT_SECURE_NO_DEPRECATE) && defined(_MSC_VER)
#define _CRT_SECURE_NO_DEPRECATE
#endif

#ifdef __GNUC__
#pragma GCC visibility push(default)
#endif
#if defined(_MSC_VER)
#pragma warning (push)
/* disable warning about single line comments in system headers */
#pragma warning (disable : 4001)
#endif

#include <string.h>
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <limits.h>
#include <ctype.h>
#include <float.h>

#ifdef ENABLE_LOCALES
#include <locale.h>
#endif

#if defined(_MSC_VER)
#pragma warning (pop)
#endif
#ifdef __GNUC__
#pragma GCC visibility pop
#endif

#include "cJSON.h"

/* C89 没有 true/false 关键字。用宏冒充。
 * 先 #undef 是防止别人的头文件已经定义过 true，造成重定义。
 * ((cJSON_bool)1) 是强制类型转换：把字面量 1 当成 cJSON_bool（其实是 int）。 */
#ifdef true
#undef true
#endif
#define true ((cJSON_bool)1)

#ifdef false
#undef false
#endif
#define false ((cJSON_bool)0)

/* NaN 的性质：任何数和自己比都相等，唯独 NaN != NaN。所以 d != d 就能判断 NaN。
 * 无穷大：inf - inf 是 NaN，但普通数减自己是 0。 */
#ifndef isinf
#define isinf(d) (isnan((d - d)) && !isnan(d))
#endif
#ifndef isnan
#define isnan(d) (d != d)
#endif

#ifndef NAN
#ifdef _WIN32
#define NAN sqrt(-1.0)
#else
#define NAN 0.0/0.0
#endif
#endif

/* 静态全局变量：整个 .c 文件共享一份，函数返回后还在。
 * json 是“基址指针”，position 是从基址走多少个 unsigned char。
 * 指针加法：json + position 得到出错那个字节的地址。 */
typedef struct {
    const unsigned char *json;
    size_t position;
} error;
static error global_error = { NULL, 0 };

CJSON_PUBLIC(const char *) cJSON_GetErrorPtr(void)
{
    /* 两个指针类型不同：json 是 unsigned char*，函数要返回 const char*，所以强制转换。
     * 转换的是“指针的类型标签”，地址数值不变。 */
    return (const char*) (global_error.json + global_error.position);
}

CJSON_PUBLIC(char *) cJSON_GetStringValue(const cJSON * const item)
{
    /* item 是“指向常量结点的常量指针”：不能 item = ...，也不能 item->type = ... */
    if (!cJSON_IsString(item))
    {
        return NULL;
    }

    /* 返回的是结点内部那块字符串的地址，不是拷贝。
     * 你若 free 这个指针，结点就变成野指针；Delete 结点后你也不能再用这个返回值。 */
    return item->valuestring;
}

CJSON_PUBLIC(double) cJSON_GetNumberValue(const cJSON * const item)
{
    if (!cJSON_IsNumber(item))
    {
        return (double) NAN;
    }

    return item->valuedouble;
}

/* 编译期断言：.h 和 .c 的版本宏必须相同，否则 #error 让编译失败。 */
#if (CJSON_VERSION_MAJOR != 1) || (CJSON_VERSION_MINOR != 7) || (CJSON_VERSION_PATCH != 19)
    #error cJSON.h and cJSON.c have different versions. Make sure that both have the same.
#endif

CJSON_PUBLIC(const char*) cJSON_Version(void)
{
    /* static 局部数组：不在栈上，函数返回后还在，所以可以返回它的地址。
     * 若写成 char version[15]; 然后 return version; 那是返回栈上即将销毁的数组，野指针。 */
    static char version[15];
    sprintf(version, "%i.%i.%i", CJSON_VERSION_MAJOR, CJSON_VERSION_MINOR, CJSON_VERSION_PATCH);

    return version;
}

/* 忽略大小写比较两个 C 字符串。返回 0 表示相等（和 strcmp 一样）。
 *
 * 参数是 unsigned char*：tolower 的参数若是负的 signed char 会出问题，
 * 所以用 unsigned 存字节更安全。
 *
 * *string1  取当前字符；string1++ 让指针走向下一个字节。 */
static int case_insensitive_strcmp(const unsigned char *string1, const unsigned char *string2)
{
    if ((string1 == NULL) || (string2 == NULL))
    {
        return 1;
    }

    /* 指针相等 = 指向同一块内存，内容一定相同，不必逐字节比。 */
    if (string1 == string2)
    {
        return 0;
    }

    /* for 的第三个表达式里 (void)string1++ ：++ 有返回值，加 (void) 表示故意丢掉，消除警告。
     * 循环条件：两个当前字符转小写后相同，才继续往后走。 */
    for(; tolower(*string1) == tolower(*string2); (void)string1++, string2++)
    {
        if (*string1 == '\0')
        {
            return 0;
        }
    }

    /* 走到第一个不同的字符，返回差值（<0 / 0 / >0），便于排序。 */
    return tolower(*string1) - tolower(*string2);
}

/* 函数指针结构体。三个成员都是“指向函数的指针”：
 *   allocate   相当于 malloc
 *   deallocate 相当于 free
 *   reallocate 相当于 realloc，可能是 NULL（自定义分配器时）
 *
 * 调用方式：hooks->allocate(32);  先用 -> 取出函数指针，再像函数一样加括号。 */
typedef struct internal_hooks
{
    void *(CJSON_CDECL *allocate)(size_t size);
    void (CJSON_CDECL *deallocate)(void *pointer);
    void *(CJSON_CDECL *reallocate)(void *pointer, size_t size);
} internal_hooks;

#if defined(_MSC_VER)
/* work around MSVC error C2322: '...' address of dllimport '...' is not static */
static void * CJSON_CDECL internal_malloc(size_t size)
{
    return malloc(size);
}
static void CJSON_CDECL internal_free(void *pointer)
{
    free(pointer);
}
static void * CJSON_CDECL internal_realloc(void *pointer, size_t size)
{
    return realloc(pointer, size);
}
#else
#define internal_malloc malloc
#define internal_free free
#define internal_realloc realloc
#endif

/* sizeof("abc") == 4（含 '\0'），sizeof("") == 1。相减得到字面量字符数 3。
 * 这是编译期算出来的，不调用 strlen。只能用于 "这种字面量"，不能用于指针。 */
#define static_strlen(string_literal) (sizeof(string_literal) - sizeof(""))

/* 文件级静态变量：所有内部 malloc 都走这里。InitHooks 会改这三个函数指针。 */
static internal_hooks global_hooks = { internal_malloc, internal_free, internal_realloc };

/* 复制一份 C 字符串到堆上（strdup 的自己实现）。
 *
 * 为什么要自己拷？原字符串可能是栈上的、或调用者马上要改的。
 * 结点必须拥有自己那份，Delete 时才能放心 free。
 *
 * 步骤：算长度（含 '\0'）→ allocate → memcpy 整段字节。 */
static unsigned char* cJSON_strdup(const unsigned char* string, const internal_hooks * const hooks)
{
    size_t length = 0;
    unsigned char *copy = NULL;

    if (string == NULL)
    {
        return NULL;
    }

    /* strlen 不含 '\0'，所以 + sizeof("")（即 +1），拷贝时连结束符一起带走。 */
    length = strlen((const char*)string) + sizeof("");
    copy = (unsigned char*)hooks->allocate(length);
    if (copy == NULL)
    {
        /* malloc 失败约定返回 NULL。调用者必须检查，否则后面 memcpy 会崩。 */
        return NULL;
    }
    memcpy(copy, string, length);

    return copy;
}

/* hooks==NULL 则恢复成系统 malloc/free。
 * 注意：如果换成了自定义 malloc/free，realloc 会被关掉（无法保证两者匹配），打印扩容会走“新分配+拷贝”。 */
CJSON_PUBLIC(void) cJSON_InitHooks(cJSON_Hooks* hooks)
{
    if (hooks == NULL)
    {
        /* Reset hooks */
        global_hooks.allocate = malloc;
        global_hooks.deallocate = free;
        global_hooks.reallocate = realloc;
        return;
    }

    global_hooks.allocate = malloc;
    if (hooks->malloc_fn != NULL)
    {
        global_hooks.allocate = hooks->malloc_fn;
    }

    global_hooks.deallocate = free;
    if (hooks->free_fn != NULL)
    {
        global_hooks.deallocate = hooks->free_fn;
    }

    /* use realloc only if both free and malloc are used */
    global_hooks.reallocate = NULL;
    if ((global_hooks.allocate == malloc) && (global_hooks.deallocate == free))
    {
        global_hooks.reallocate = realloc;
    }
}

/* 堆上分配一个结点，并把每个字节写成 0。
 *
 * sizeof(cJSON) 是整个结构体占多少字节（含所有指针和数字字段）。
 * malloc 返回的是“未初始化的原始内存”，里面是垃圾值。
 * 若不 memset，next/child 可能是随机地址，后面一解引用就崩。
 *
 * memset(node, '\0', n) 把 n 个字节都写成 0。对指针来说 0 就是 NULL。
 * if (node) 等价于 if (node != NULL)。 */
static cJSON *cJSON_New_Item(const internal_hooks * const hooks)
{
    cJSON* node = (cJSON*)hooks->allocate(sizeof(cJSON));
    if (node)
    {
        memset(node, '\0', sizeof(cJSON));
    }

    return node;
}

/* 释放一棵树。这是学“指针所有权”最好的函数。
 *
 * 内存图像：Delete(根) 必须
 *   1. 递归 Delete(child)     —— 子树
 *   2. 循环 item = item->next —— 兄弟（数组里 1,2,3 是兄弟，不是父子）
 *   3. free 自己的 valuestring / string / 结点本身
 *
 * 为什么先 next = item->next？
 *   因为马上要把 item 这块内存 free 掉。free 之后 item->next 就是野指针，
 *   必须提前把“下一个地址”保存在栈上的局部变量 next 里。
 *
 * type & 标志：按位与。该位是 1 表示“内存不是我的，别 free”。
 * deallocate 之后立刻 = NULL：防止同一指针被 free 两次（double free）。 */
CJSON_PUBLIC(void) cJSON_Delete(cJSON *item)
{
    cJSON *next = NULL;
    while (item != NULL)
    {
        next = item->next;
        if (!(item->type & cJSON_IsReference) && (item->child != NULL))
        {
            cJSON_Delete(item->child);
        }
        if (!(item->type & cJSON_IsReference) && (item->valuestring != NULL))
        {
            global_hooks.deallocate(item->valuestring);
            item->valuestring = NULL;
        }
        if (!(item->type & cJSON_StringIsConst) && (item->string != NULL))
        {
            global_hooks.deallocate(item->string);
            item->string = NULL;
        }
        global_hooks.deallocate(item);
        item = next;
    }
}

/* get the decimal point character of the current locale */
static unsigned char get_decimal_point(void)
{
#ifdef ENABLE_LOCALES
    struct lconv *lconv = localeconv();
    return (unsigned char) lconv->decimal_point[0];
#else
    return '.';
#endif
}

/* parse_buffer：解析时的“光标 + 整段输入”。
 *
 * content : 指向调用者那串 JSON 的第一个字节（我们不拥有这块内存，只读）
 * length  : 一共多少字节
 * offset  : 当前读到第几个字节（从 0 开始）
 * depth   : 嵌套了几层 '[' 或 '{'
 *
 * 当前字符的地址 = content + offset
 * 这就是指针加法：unsigned char* + size_t → 往后跳 offset 个字节。 */
typedef struct
{
    const unsigned char *content;
    size_t length;
    size_t offset;
    size_t depth;
    internal_hooks hooks;
} parse_buffer;

/* 宏是“文本替换”，没有函数调用开销，也没有类型检查。
 *
 * (buffer)->offset  必须加括号：如果有人写 can_read(p+1, 4)，没有括号会算错。
 * && 短路：左边已经是假，右边根本不执行，所以 buffer==NULL 时不会去读 buffer->offset。 */
#define can_read(buffer, size) ((buffer != NULL) && (((buffer)->offset + size) <= (buffer)->length))
#define can_access_at_index(buffer, index) ((buffer != NULL) && (((buffer)->offset + index) < (buffer)->length))
#define cannot_access_at_index(buffer, index) (!can_access_at_index(buffer, index))
/* 得到“当前光标”指针。后面 [0] 就是当前字符，[1] 就是下一个。
 * 数组下标 a[i] 本质是 *(a+i)。所以 p[0] 就是 *p。 */
#define buffer_at_offset(buffer) ((buffer)->content + (buffer)->offset)

/* 解析数字到 item->valuedouble / valueint。
 *
 * 输入可能没有 '\0'（ParseWithLength），不能直接 strtod(原串)。
 * 做法：把数字字符拷到自己 malloc 的临时缓冲，补上 '\0'，再 strtod。
 *
 * after_end：strtod 的第二个参数是 char**，它会把“数字结束位置”写进去。
 *   (char**)&after_end  取 after_end 这个指针变量自己的地址，再转成 char**。
 *   这是“输出参数”：函数通过修改你的指针变量，告诉你解析停在哪。
 *
 * after_end - number_c_string：两个指针相减 = 中间隔了多少个 unsigned char，
 * 也就是数字占了几个字符。用这个推进 input_buffer->offset。 */
static cJSON_bool parse_number(cJSON * const item, parse_buffer * const input_buffer)
{
    double number = 0;                          /* 最终解析出的浮点值，先清零 */
    unsigned char *after_end = NULL;            /* strtod 会把它改成“数字结束处”的地址 */
    unsigned char *number_c_string;             /* 指向堆上临时拷贝的那串数字（带 '\0'） */
    unsigned char decimal_point = get_decimal_point(); /* 当前 locale 的小数点，多数环境是 '.' */
    size_t i = 0;                               /* 循环下标：从当前光标往后扫第几个字节 */
    size_t number_string_length = 0;            /* 数字占了几个字符，后面用来 malloc / memcpy */
    cJSON_bool has_decimal_point = false;       /* 有没有见到 '.'，决定要不要替换 locale 小数点 */

    /* 空指针防护：|| 短路，左边已经真就不再读右边，避免 input_buffer 为 NULL 时解引用崩溃 */
    if ((input_buffer == NULL) || (input_buffer->content == NULL))
    {
        return false;                           /* 解析失败：没有输入 */
    }

    /* 先量出数字有多长。不能直接 strtod(原串)：ParseWithLength 的输入可能没有 '\0'。
     * for 三部分：初始化 i=0；每次判断“光标+i 还在缓冲里”；循环末尾 i++。 */
    for (i = 0; can_access_at_index(input_buffer, i); i++)
    {
        /* buffer_at_offset = content + offset，再 [i] 就是当前光标往后第 i 个字节。
         * switch 按这个字符分类，多个 case 落到同一段代码叫“贯穿”。 */
        switch (buffer_at_offset(input_buffer)[i])
        {
            case '0':                           /* JSON 数字允许的字符：0-9、正负号、科学计数 e/E */
            case '1':
            case '2':
            case '3':
            case '4':
            case '5':
            case '6':
            case '7':
            case '8':
            case '9':
            case '+':
            case '-':
            case 'e':
            case 'E':
                number_string_length++;         /* 这个字符算进数字长度 */
                break;                          /* 跳出 switch（不是跳出 for），去 for 的 i++ */

            case '.':                           /* JSON 小数点永远是 '.'，和 locale 无关 */
                number_string_length++;         /* '.' 也是数字的一部分，长度 +1 */
                has_decimal_point = true;       /* 记下：后面拷贝完要把 '.' 换成 locale 小数点 */
                break;

            default:
                /* 遇到逗号、空格、] 等“不是数字的字符”，数字结束。
                 * break 只能跳出 switch；要跳出 for 用 goto 跳到循环后的标签。 */
                goto loop_end;
        }
    }
loop_end:                                       /* 标签：for 正常走完（撞到缓冲末尾）或 goto 都会到这里 */
    /* hooks.allocate 一般就是 malloc。+1 是给结尾 '\0' 留一字节。
     * (unsigned char *) 把 void* 转成我们要用的指针类型。 */
    number_c_string = (unsigned char *) input_buffer->hooks.allocate(number_string_length + 1);
    if (number_c_string == NULL)                /* malloc 失败返回 NULL，必须检查 */
    {
        return false;                           /* 分配失败，同样算解析失败 */
    }

    /* 从原 JSON 光标处拷 number_string_length 个字节到临时缓冲（还没有 '\0'） */
    memcpy(number_c_string, buffer_at_offset(input_buffer), number_string_length);
    number_c_string[number_string_length] = '\0'; /* 自己补上结尾 0，C 字符串才合法，strtod 才知道停哪 */

    if (has_decimal_point)                      /* 没有小数点就不必扫第二遍 */
    {
        for (i = 0; i < number_string_length; i++) /* 在拷贝里找 '.' */
        {
            if (number_c_string[i] == '.')      /* 找到 JSON 的小数点 */
            {
                /* strtod 按 locale 认小数点：德语环境是 ','。换成它，strtod 才认得出。 */
                number_c_string[i] = decimal_point;
            }
        }
    }

    /* strtod：把 C 字符串转成 double。第二个参数是 char**（输出参数）：
     * 传入 after_end 这个指针变量的地址，strtod 会把“停下来的位置”写进去。
     * (const char*) 和 (char**) 是类型转换，因为临时缓冲是 unsigned char*。 */
    number = strtod((const char*)number_c_string, (char**)&after_end);
    if (number_c_string == after_end)           /* 两个地址相同 = 一个数字字符都没吃掉 */
    {
        input_buffer->hooks.deallocate(number_c_string); /* 失败也要释放，否则泄漏 */
        return false;                           /* 不是合法数字，例如输入是 "true" */
    }

    item->valuedouble = number;                 /* 完整精度写进结点的 double 字段 */

    /* 再填一份 int。超大 double 直接 (int) 是未定义行为，先和 INT_MAX/INT_MIN 比较，溢出就夹到边界。 */
    if (number >= INT_MAX)                      /* INT_MAX 是 int 能表示的最大值（通常 2147483647） */
    {
        item->valueint = INT_MAX;               /* 饱和：太大就记成 int 上限 */
    }
    else if (number <= (double)INT_MIN)         /* INT_MIN 先转成 double 再比，避免 int 比较的坑 */
    {
        item->valueint = INT_MIN;               /* 饱和：太小就记成 int 下限 */
    }
    else
    {
        item->valueint = (int)number;           /* 范围内：截断小数部分，1.9 变成 1 */
    }

    item->type = cJSON_Number;                  /* 标明这个结点是数字类型 */

    /* after_end - number_c_string = 临时串里被 strtod 吃掉的字符数（指针相减）。
     * 加到 offset 上，光标移到数字后面那个字符（例如逗号或 }）。 */
    input_buffer->offset += (size_t)(after_end - number_c_string);
    input_buffer->hooks.deallocate(number_c_string); /* 临时缓冲用完立刻释放 */
    return true;                                /* 成功：item 已填好，光标已前进 */
}

/* don't ask me, but the original cJSON_SetNumberValue returns an integer or double */
CJSON_PUBLIC(double) cJSON_SetNumberHelper(cJSON *object, double number)
{
    if (object == NULL)
    {
        return (double)NAN;
    }

    if (number >= INT_MAX)
    {
        object->valueint = INT_MAX;
    }
    else if (number <= (double)INT_MIN)
    {
        object->valueint = INT_MIN;
    }
    else
    {
        object->valueint = (int)number;
    }

    return object->valuedouble = number;
}

/* Note: when passing a NULL valuestring, cJSON_SetValuestring treats this as an error and return NULL */
CJSON_PUBLIC(char*) cJSON_SetValuestring(cJSON *object, const char *valuestring)
{
    char *copy = NULL;
    size_t v1_len;
    size_t v2_len;
    /* if object's type is not cJSON_String or is cJSON_IsReference, it should not set valuestring */
    if ((object == NULL) || !(object->type & cJSON_String) || (object->type & cJSON_IsReference))
    {
        return NULL;
    }
    /* return NULL if the object is corrupted or valuestring is NULL */
    if (object->valuestring == NULL || valuestring == NULL)
    {
        return NULL;
    }

    v1_len = strlen(valuestring);
    v2_len = strlen(object->valuestring);

    if (v1_len <= v2_len)
    {
        /* 指针比较：两个 char* 比的是地址大小。
         * 若两段内存重叠，strcpy 行为未定义。这里要求一段完全在另一段左边或右边。 */
        if (!( valuestring + v1_len < object->valuestring || object->valuestring + v2_len < valuestring ))
        {
            return NULL;
        }
        strcpy(object->valuestring, valuestring);
        return object->valuestring;
    }
    copy = (char*) cJSON_strdup((const unsigned char*)valuestring, &global_hooks);
    if (copy == NULL)
    {
        return NULL;
    }
    if (object->valuestring != NULL)
    {
        cJSON_free(object->valuestring);
    }
    object->valuestring = copy;

    return copy;
}

/* printbuffer：可扩容的输出数组（C 没有 vector，都是自己用指针+长度模拟）。
 *
 * buffer : 堆上（或调用者提供）的 char 数组首地址
 * length : 当前容量（能装多少字节）
 * offset : 已经写了多少字节，下一个字写在 buffer[offset]
 * noalloc: 1 就不能 realloc，满了只能失败
 *
 * 关系：永远应有 offset < length，并留 1 字节给 '\0'。 */
typedef struct
{
    unsigned char *buffer;
    size_t length;
    size_t offset;
    size_t depth; /* current nesting depth (for formatted printing) */
    cJSON_bool noalloc;
    cJSON_bool format; /* is this print a formatted print */
    internal_hooks hooks;
} printbuffer;

/* 保证从当前 offset 起还能再写 needed 字节。
 *
 * 返回值：p->buffer + p->offset，即“现在该往哪写”。
 * 调用者拿到后直接 *ptr = '[' 或 strcpy(ptr, "null")。
 *
 * 扩容：新容量约 needed*2。realloc 可能返回新地址，旧 buffer 失效，
 * 所以必须 p->buffer = newbuffer 更新结构体里的指针。
 *
 * 没有 realloc 时：allocate 新块 → memcpy 旧内容 → deallocate 旧块。
 * 这就是“手动 realloc”。 */
static unsigned char* ensure(printbuffer * const p, size_t needed)
{
    unsigned char *newbuffer = NULL;
    size_t newsize = 0;

    if ((p == NULL) || (p->buffer == NULL))
    {
        return NULL;
    }

    if ((p->length > 0) && (p->offset >= p->length))
    {
        /* make sure that offset is valid */
        return NULL;
    }

    if (needed > INT_MAX)
    {
        /* sizes bigger than INT_MAX are currently not supported */
        return NULL;
    }

    needed += p->offset + 1;
    if (needed <= p->length)
    {
        /* 容量够：返回当前写入点。注意这是指针加法，不是下标变量。 */
        return p->buffer + p->offset;
    }

    if (p->noalloc) {
        /* 调用者禁止扩容（PrintPreallocated）。只能失败，不能越界写。 */
        return NULL;
    }

    /* calculate new buffer size */
    if (needed > (INT_MAX / 2))
    {
        /* overflow of int, use INT_MAX if possible */
        if (needed <= INT_MAX)
        {
            newsize = INT_MAX;
        }
        else
        {
            return NULL;
        }
    }
    else
    {
        newsize = needed * 2;
    }

    if (p->hooks.reallocate != NULL)
    {
        /* reallocate with realloc if available */
        newbuffer = (unsigned char*)p->hooks.reallocate(p->buffer, newsize);
        if (newbuffer == NULL)
        {
            p->hooks.deallocate(p->buffer);
            p->length = 0;
            p->buffer = NULL;

            return NULL;
        }
    }
    else
    {
        /* otherwise reallocate manually */
        newbuffer = (unsigned char*)p->hooks.allocate(newsize);
        if (!newbuffer)
        {
            p->hooks.deallocate(p->buffer);
            p->length = 0;
            p->buffer = NULL;

            return NULL;
        }

        memcpy(newbuffer, p->buffer, p->offset + 1);
        p->hooks.deallocate(p->buffer);
    }
    p->length = newsize;
    p->buffer = newbuffer;

    return newbuffer + p->offset;
}

/* calculate the new length of the string in a printbuffer and update the offset */
static void update_offset(printbuffer * const buffer)
{
    const unsigned char *buffer_pointer = NULL;
    if ((buffer == NULL) || (buffer->buffer == NULL))
    {
        return;
    }
    buffer_pointer = buffer->buffer + buffer->offset;

    buffer->offset += strlen((const char*)buffer_pointer);
}

/* securely comparison of floating-point variables */
static cJSON_bool compare_double(double a, double b)
{
    double maxVal = fabs(a) > fabs(b) ? fabs(a) : fabs(b);
    return (fabs(a - b) <= maxVal * DBL_EPSILON);
}

/* 把数字写成 JSON 文本。NaN/Inf 不是合法 JSON，这里打印成 null。
 * 整数用 %d；小数先试 15 位有效数字，用 sscanf 读回来对不上再改用 17 位（IEEE754 往返）。 */
static cJSON_bool print_number(const cJSON * const item, printbuffer * const output_buffer)
{
    unsigned char *output_pointer = NULL;
    double d = item->valuedouble;
    int length = 0;
    size_t i = 0;
    unsigned char number_buffer[26] = {0}; /* temporary buffer to print the number into */
    unsigned char decimal_point = get_decimal_point();
    double test = 0.0;

    if (output_buffer == NULL)
    {
        return false;
    }

    /* This checks for NaN and Infinity */
    if (isnan(d) || isinf(d))
    {
        length = sprintf((char*)number_buffer, "null");
    }
    else if(d == (double)item->valueint)
    {
        length = sprintf((char*)number_buffer, "%d", item->valueint);
    }
    else
    {
        /* Try 15 decimal places of precision to avoid nonsignificant nonzero digits */
        length = sprintf((char*)number_buffer, "%1.15g", d);

        /* Check whether the original double can be recovered */
        if ((sscanf((char*)number_buffer, "%lg", &test) != 1) || !compare_double((double)test, d))
        {
            /* If not, print with 17 decimal places of precision */
            length = sprintf((char*)number_buffer, "%1.17g", d);
        }
    }

    /* sprintf failed or buffer overrun occurred */
    if ((length < 0) || (length > (int)(sizeof(number_buffer) - 1)))
    {
        return false;
    }

    /* reserve appropriate space in the output */
    output_pointer = ensure(output_buffer, (size_t)length + sizeof(""));
    if (output_pointer == NULL)
    {
        return false;
    }

    /* copy the printed number to the output and replace locale
     * dependent decimal point with '.' */
    for (i = 0; i < ((size_t)length); i++)
    {
        if (number_buffer[i] == decimal_point)
        {
            output_pointer[i] = '.';
            continue;
        }

        output_pointer[i] = number_buffer[i];
    }
    output_pointer[i] = '\0';

    output_buffer->offset += (size_t)length;

    return true;
}

/* 把 4 个十六进制字符变成整数，例如 "0041" -> 0x41。用于 JSON 的 \uXXXX。 */
static unsigned parse_hex4(const unsigned char * const input)
{
    unsigned int h = 0;
    size_t i = 0;

    for (i = 0; i < 4; i++)
    {
        /* parse digit */
        if ((input[i] >= '0') && (input[i] <= '9'))
        {
            h += (unsigned int) input[i] - '0';
        }
        else if ((input[i] >= 'A') && (input[i] <= 'F'))
        {
            h += (unsigned int) 10 + input[i] - 'A';
        }
        else if ((input[i] >= 'a') && (input[i] <= 'f'))
        {
            h += (unsigned int) 10 + input[i] - 'a';
        }
        else /* invalid */
        {
            return 0;
        }

        if (i < 3)
        {
            /* << 4 等于乘 16：给下一个十六进制位腾出低 4 比特。
             * 例如已读 'A'(10)，左移后变成 0xA0，再加下一个数字。 */
            h = h << 4;
        }
    }

    return h;
}

/* JSON 字符串里的 \uXXXX 是 UTF-16 码元。C 里我们要存 UTF-8。
 * 基本多文种平面用一个 \uXXXX；emoji 等需要代理对 \uD83D\uDE00 两个拼成一个码点，再编码成 1~4 字节 UTF-8。
 * 成功返回消耗的输入字节数（6 或 12），失败返回 0。 */
static unsigned char utf16_literal_to_utf8(const unsigned char * const input_pointer, const unsigned char * const input_end, unsigned char **output_pointer)
{
    long unsigned int codepoint = 0;            /* 最终那个 Unicode 码点（一个整数，不是字节） */
    unsigned int first_code = 0;                /* 第一个 \uXXXX 解出来的 16 位值 */
    const unsigned char *first_sequence = input_pointer; /* 记住 '\' 的位置；后面 +2 才是四个十六进制 */
    unsigned char utf8_length = 0;              /* 这个码点要写成几个 UTF-8 字节：1~4 */
    unsigned char utf8_position = 0;            /* 从后往前填 UTF-8 后续字节时用的下标 */
    unsigned char sequence_length = 0;          /* 输入侧吃掉多少字节：6 或 12，成功后返回给调用者 */
    unsigned char first_byte_mark = 0;          /* 多字节 UTF-8 首字节的高位标记：110 / 1110 / 11110 */

    if ((input_end - first_sequence) < 6)       /* \uXXXX 一共 6 个字节；指针相减看还剩几个 */
    {
        goto fail;                              /* 输入在半路上断了，例如 "\u12 */
    }

    first_code = parse_hex4(first_sequence + 2); /* 跳过 '\''u'，把后面 4 个十六进制字符收成整数 */

    /* 0xDC00~0xDFFF 是“低代理”，只能跟在高代理后面，单独出现非法 */
    if (((first_code >= 0xDC00) && (first_code <= 0xDFFF)))
    {
        goto fail;
    }

    /* 0xD800~0xDBFF 是“高代理”：后面还必须再跟一个 \uXXXX（低代理），两半拼成一个大码点 */
    if ((first_code >= 0xD800) && (first_code <= 0xDBFF))
    {
        const unsigned char *second_sequence = first_sequence + 6; /* 第二个 '\' 应该在这里 */
        unsigned int second_code = 0;           /* 低代理那 16 位 */
        sequence_length = 12;                   /* 两个 \uXXXX，输入共 12 字节 */

        if ((input_end - second_sequence) < 6)  /* 高代理后面不够再读 6 字节 */
        {
            goto fail;
        }

        if ((second_sequence[0] != '\\') || (second_sequence[1] != 'u'))
        {
            goto fail;                          /* 后面不是 \u，代理对缺了一半 */
        }

        second_code = parse_hex4(second_sequence + 2); /* 再读低代理的四个十六进制 */
        if ((second_code < 0xDC00) || (second_code > 0xDFFF))
        {
            goto fail;                          /* 第二段必须是低代理，不能是普通码元或又一个高代理 */
        }

        /* 公式（UTF-16）：码点 = 0x10000 + (高10位 << 10) | 低10位
         * & 0x3FF 取出低 10 比特；高代理左移 10 位后和低代理按位或。
         * 例如 😀 是 \uD83D\uDE00 → 0x1F600。 */
        codepoint = 0x10000 + (((first_code & 0x3FF) << 10) | (second_code & 0x3FF));
    }
    else
    {
        sequence_length = 6;                    /* 普通 BMP 字符，一个 \uXXXX 就够 */
        codepoint = first_code;                 /* 码点就是这 16 位本身，例如 \u0041 → 'A' */
    }

    /* 下面按码点大小决定 UTF-8 写几字节。UTF-8 最多 4 字节：
     * 1 字节: 0xxxxxxx
     * 2 字节: 110xxxxx 10xxxxxx
     * 3 字节: 1110xxxx 10xxxxxx 10xxxxxx
     * 4 字节: 11110xxx 10xxxxxx 10xxxxxx 10xxxxxx */
    if (codepoint < 0x80)                       /* ASCII：0~127，一个字节原样放下 */
    {
        utf8_length = 1;
    }
    else if (codepoint < 0x800)                 /* 到 11 比特：拉丁扩展、希腊语等 */
    {
        utf8_length = 2;
        first_byte_mark = 0xC0;                 /* 11000000，给首字节贴上 110 前缀 */
    }
    else if (codepoint < 0x10000)               /* 到 16 比特：中文、日文等 BMP */
    {
        utf8_length = 3;
        first_byte_mark = 0xE0;                 /* 11100000 */
    }
    else if (codepoint <= 0x10FFFF)             /* Unicode 上限；emoji 走这里，4 字节 */
    {
        utf8_length = 4;
        first_byte_mark = 0xF0;                 /* 11110000 */
    }
    else
    {
        goto fail;                              /* 超出 Unicode 范围，不可能合法 */
    }

    /* 从后往前填“后续字节”。每个后续字节格式是 10xxxxxx：
     * | 0x80 把最高两位置成 10，& 0xBF 清掉第 6 位，保证是 10xxxxxx。
     * 填完一次，codepoint 右移 6 位，露出下一组要写的比特。 */
    for (utf8_position = (unsigned char)(utf8_length - 1); utf8_position > 0; utf8_position--)
    {
        (*output_pointer)[utf8_position] = (unsigned char)((codepoint | 0x80) & 0xBF);
        codepoint >>= 6;                        /* 丢掉已经写进后续字节的那 6 比特 */
    }
    if (utf8_length > 1)                        /* 多字节：首字节 = 剩下的低位 | 110/1110/11110 标记 */
    {
        (*output_pointer)[0] = (unsigned char)((codepoint | first_byte_mark) & 0xFF);
    }
    else
    {
        (*output_pointer)[0] = (unsigned char)(codepoint & 0x7F); /* 单字节：只留低 7 位 */
    }

    /* output_pointer 是 unsigned char**。*output_pointer 才是 parse_string 里的写指针。
     * 写完 n 字节后把它往前挪 n，调用者才能接着写后面的字符。 */
    *output_pointer += utf8_length;

    return sequence_length;                     /* 告诉 parse_string：输入要跳过 6 或 12 字节 */

fail:
    return 0;                                   /* 约定：0 表示失败，调用者 goto fail */
}

/* 解析 JSON 字符串字面量。
 *
 * 输入：光标停在开头的 '"' 上。
 * 输出：item->valuestring 指向堆上新字符串（已去掉引号、已解码转义）。
 *
 * 两个指针一起走：
 *   input_pointer  读原 JSON
 *   output_pointer 写解码后的内容
 * 它们可以差得很多：\n 在输入占 2 字节，输出只占 1 字节。
 *
 * input_end - input_buffer->content  指针相减得到“从开头数第几个字节”，
 * 用来和 length 比较，防止读出界。 */
static cJSON_bool parse_string(cJSON * const item, parse_buffer * const input_buffer)
{
    /* 两个读指针都从开头 '"' 的下一个字节起步。
     * input_end 第一遍用来量长度（找到收尾 '"'），input_pointer 第二遍才真正解码。 */
    const unsigned char *input_pointer = buffer_at_offset(input_buffer) + 1;
    const unsigned char *input_end = buffer_at_offset(input_buffer) + 1;
    unsigned char *output_pointer = NULL;       /* 往堆上结果里写的光标，第二遍才用 */
    unsigned char *output = NULL;               /* 堆上结果的起点；失败时靠它判断要不要 free */

    if (buffer_at_offset(input_buffer)[0] != '\"') /* 光标必须停在开头的双引号上 */
    {
        goto fail;                              /* 不是字符串 */
    }

    {                                           /* 独立块：第一遍的局部变量离开大括号就没了 */
        size_t allocation_length = 0;           /* 准备 malloc 多少字节（偏大一点也没关系） */
        size_t skipped_bytes = 0;               /* 转义里“多出来、输出不需要”的字节数，主要是每个 '\' */
        /* 没越界，且还没碰到未转义的收尾 '"'，就继续往前探。
         * input_end - content：指针相减 = 从整段 JSON 开头数过了几个字节。 */
        while (((size_t)(input_end - input_buffer->content) < input_buffer->length) && (*input_end != '\"'))
        {
            if (input_end[0] == '\\')           /* 见到反斜杠：下一个字符是转义，不是收尾引号 */
            {
                /* '\' 已经是最后一个字节，后面没有可配对的字符，例如 "abc\ */
                if ((size_t)(input_end + 1 - input_buffer->content) >= input_buffer->length)
                {
                    goto fail;                  /* 防止再读 input_end[1] 越界 */
                }
                skipped_bytes++;                /* 输出里不会留下这个 '\'，长度可以少算 1 */
                input_end++;                    /* 先跳过 '\'，下面还会再 ++，等于连转义符一起跳过 */
            }
            input_end++;                        /* 普通字符或转义的第二个字符，都往前走一格 */
        }
        /* 走到缓冲末尾还没见到 '"'，或当前不是 '"'（理论上后者被 while 挡住了） */
        if (((size_t)(input_end - input_buffer->content) >= input_buffer->length) || (*input_end != '\"'))
        {
            goto fail;                          /* 字符串没闭合，例如 "abc */
        }

        /* 从开头 '"' 到收尾 '"' 的跨度，减去那些 '\'。
         * 这是上限：\n 输入 2 字节输出 1；\uXXXX 输入 6 输出最多 4，多分配一点没关系。 */
        allocation_length = (size_t) (input_end - buffer_at_offset(input_buffer)) - skipped_bytes;
        /* + sizeof("") 即 +1，给结尾 '\0'。hooks.allocate 一般就是 malloc */
        output = (unsigned char*)input_buffer->hooks.allocate(allocation_length + sizeof(""));
        if (output == NULL)                     /* 分配失败 */
        {
            goto fail;
        }
    }

    output_pointer = output;                    /* 写指针回到结果开头，开始第二遍：真正解码 */
    while (input_pointer < input_end)           /* 一直写到收尾 '"' 之前（input_end 正指着那个 '"'） */
    {
        if (*input_pointer != '\\')             /* 普通字符，原样拷到输出 */
        {
            /* *dst++ = *src++：先把 *src 写成 *dst，然后两个指针各 +1 */
            *output_pointer++ = *input_pointer++;
        }
        else                                    /* 当前是 '\'，看下一个字符决定写成什么 */
        {
            unsigned char sequence_length = 2;  /* 默认转义占 2 字节：\n \t \" \\ 等 */
            if ((input_end - input_pointer) < 1) /* 只剩 '\' 没有后继（第一遍其实已拦过） */
            {
                goto fail;
            }

            switch (input_pointer[1])           /* 看 '\' 后面那一个字符 */
            {
                case 'b':
                    *output_pointer++ = '\b';   /* 退格，ASCII 8；输入 2 字节，输出 1 字节 */
                    break;
                case 'f':
                    *output_pointer++ = '\f';   /* 换页，ASCII 12 */
                    break;
                case 'n':
                    *output_pointer++ = '\n';   /* 换行。JSON 文本里是反斜杠+n，内存里是一个 0x0A */
                    break;
                case 'r':
                    *output_pointer++ = '\r';   /* 回车 */
                    break;
                case 't':
                    *output_pointer++ = '\t';   /* 制表符 */
                    break;
                case '\"':                      /* \"  → 一个双引号字符 */
                case '\\':                      /* \\  → 一个反斜杠 */
                case '/':                       /* \/  → 一个斜杠（JSON 允许这样写，方便 </script>） */
                    *output_pointer++ = input_pointer[1]; /* 输出就是 '\' 后面那个字符本身 */
                    break;

                case 'u':                       /* \uXXXX：UTF-16 码元，要转成 UTF-8 再写入 */
                    /* &output_pointer：传“写指针变量的地址”，函数内部会把写指针往前挪 */
                    sequence_length = utf16_literal_to_utf8(input_pointer, input_end, &output_pointer);
                    if (sequence_length == 0)   /* 0 表示 \u 非法，或代理对不完整 */
                    {
                        goto fail;
                    }
                    break;                      /* sequence_length 是 6（一个 \uXXXX）或 12（代理对） */

                default:
                    goto fail;                  /* JSON 不允许的转义，例如 \x \a */
            }
            input_pointer += sequence_length;   /* 读指针一次跳过整个转义序列，不要只 +1 */
        }
    }

    *output_pointer = '\0';                     /* C 字符串必须自己补结束符，strlen/printf 才认 */

    item->type = cJSON_String;                  /* 标明这个结点是字符串 */
    item->valuestring = (char*)output;          /* 结点拥有这块堆内存，Delete 时会 free */

    /* input_end 指着收尾 '"'。减去整段 JSON 基址，得到这个 '"' 的下标 */
    input_buffer->offset = (size_t) (input_end - input_buffer->content);
    input_buffer->offset++;                     /* 再 +1，光标停在收尾引号后面（逗号、} 等） */

    return true;

fail:
    if (output != NULL)                         /* 已经 malloc 过（可能只分配了还没写完） */
    {
        input_buffer->hooks.deallocate(output); /* 释放半成品，避免泄漏 */
        output = NULL;                          /* 立刻置空，防止后面再 free 一次 */
    }

    if (input_pointer != NULL)                  /* 本函数里它一开始就被赋值了，这条几乎总是真 */
    {
        /* 失败时光标尽量停在出错处，GetErrorPtr 才能指到问题字符 */
        input_buffer->offset = (size_t)(input_pointer - input_buffer->content);
    }

    return false;
}

/* 把内存里的 C 字符串写成 JSON 字符串：加上引号，并把 " \ 和控制字符转义成 \n \u0001 等。
 * 这是 parse_string 的逆过程。 */
static cJSON_bool print_string_ptr(const unsigned char * const input, printbuffer * const output_buffer)
{
    const unsigned char *input_pointer = NULL;
    unsigned char *output = NULL;
    unsigned char *output_pointer = NULL;
    size_t output_length = 0;
    /* numbers of additional characters needed for escaping */
    size_t escape_characters = 0;

    if (output_buffer == NULL)
    {
        return false;
    }

    /* empty string */
    if (input == NULL)
    {
        output = ensure(output_buffer, sizeof("\"\""));
        if (output == NULL)
        {
            return false;
        }
        strcpy((char*)output, "\"\"");

        return true;
    }

    /* set "flag" to 1 if something needs to be escaped */
    for (input_pointer = input; *input_pointer; input_pointer++)
    {
        switch (*input_pointer)
        {
            case '\"':
            case '\\':
            case '\b':
            case '\f':
            case '\n':
            case '\r':
            case '\t':
                /* one character escape sequence */
                escape_characters++;
                break;
            default:
                if (*input_pointer < 32)
                {
                    /* UTF-16 escape sequence uXXXX */
                    escape_characters += 5;
                }
                break;
        }
    }
    output_length = (size_t)(input_pointer - input) + escape_characters;

    output = ensure(output_buffer, output_length + sizeof("\"\""));
    if (output == NULL)
    {
        return false;
    }

    /* no characters have to be escaped */
    if (escape_characters == 0)
    {
        output[0] = '\"';
        memcpy(output + 1, input, output_length);
        output[output_length + 1] = '\"';
        output[output_length + 2] = '\0';

        return true;
    }

    output[0] = '\"';
    output_pointer = output + 1;
    /* copy the string */
    for (input_pointer = input; *input_pointer != '\0'; (void)input_pointer++, output_pointer++)
    {
        if ((*input_pointer > 31) && (*input_pointer != '\"') && (*input_pointer != '\\'))
        {
            /* normal character, copy */
            *output_pointer = *input_pointer;
        }
        else
        {
            /* character needs to be escaped */
            *output_pointer++ = '\\';
            switch (*input_pointer)
            {
                case '\\':
                    *output_pointer = '\\';
                    break;
                case '\"':
                    *output_pointer = '\"';
                    break;
                case '\b':
                    *output_pointer = 'b';
                    break;
                case '\f':
                    *output_pointer = 'f';
                    break;
                case '\n':
                    *output_pointer = 'n';
                    break;
                case '\r':
                    *output_pointer = 'r';
                    break;
                case '\t':
                    *output_pointer = 't';
                    break;
                default:
                    /* escape and print as unicode codepoint */
                    sprintf((char*)output_pointer, "u%04x", *input_pointer);
                    output_pointer += 4;
                    break;
            }
        }
    }
    output[output_length + 1] = '\"';
    output[output_length + 2] = '\0';

    return true;
}

/* Invoke print_string_ptr (which is useful) on an item. */
static cJSON_bool print_string(const cJSON * const item, printbuffer * const p)
{
    return print_string_ptr((unsigned char*)item->valuestring, p);
}

/* Predeclare these prototypes. */
static cJSON_bool parse_value(cJSON * const item, parse_buffer * const input_buffer);
static cJSON_bool print_value(const cJSON * const item, printbuffer * const output_buffer);
static cJSON_bool parse_array(cJSON * const item, parse_buffer * const input_buffer);
static cJSON_bool print_array(const cJSON * const item, printbuffer * const output_buffer);
static cJSON_bool parse_object(cJSON * const item, parse_buffer * const input_buffer);
static cJSON_bool print_object(const cJSON * const item, printbuffer * const output_buffer);

/* 跳过 ASCII 码 <= 32 的空白（空格 32、Tab 9、CR 13、LF 10）。
 * 只增加 offset，不移动 content。content 始终指向整段输入的开头。 */
static parse_buffer *buffer_skip_whitespace(parse_buffer * const buffer)
{
    if ((buffer == NULL) || (buffer->content == NULL))
    {
        return NULL;
    }

    if (cannot_access_at_index(buffer, 0))
    {
        return buffer;
    }

    while (can_access_at_index(buffer, 0) && (buffer_at_offset(buffer)[0] <= 32))
    {
       buffer->offset++;
    }

    if (buffer->offset == buffer->length)
    {
        buffer->offset--;
    }

    return buffer;
}

/* UTF-8 BOM 是文件开头三个字节 EF BB BF，有的编辑器会加上。
 * strncmp 比较原始字节。只在 offset==0（文件开头）时才跳。 */
static parse_buffer *skip_utf8_bom(parse_buffer * const buffer)
{
    if ((buffer == NULL) || (buffer->content == NULL) || (buffer->offset != 0))
    {
        return NULL;
    }

    if (can_access_at_index(buffer, 4) && (strncmp((const char*)buffer_at_offset(buffer), "\xEF\xBB\xBF", 3) == 0))
    {
        buffer->offset += 3;
    }

    return buffer;
}

CJSON_PUBLIC(cJSON *) cJSON_ParseWithOpts(const char *value, const char **return_parse_end, cJSON_bool require_null_terminated)
{
    size_t buffer_length;

    if (NULL == value)
    {
        return NULL;
    }

    /* strlen 不含 '\0'。sizeof("")==1，加回来让 length 覆盖结束符。
     * 这样 require_null_terminated 时能检查到末尾的 '\0'。 */
    buffer_length = strlen(value) + sizeof("");

    return cJSON_ParseWithLengthOpts(value, buffer_length, return_parse_end, require_null_terminated);
}

/* 真正干活的解析入口。
 *
 * 参数里的 const char **return_parse_end 是“指针的指针”：
 *   调用者有一个 const char *end;  传 &end
 *   函数里 *return_parse_end = 某个地址;  于是调用者的 end 被改掉
 * 这是 C 里“函数要修改调用者的指针变量”的标准手法。只传 const char* 只能改字符，改不了那个指针变量本身。
 *
 * goto fail：C 没有 try/finally。出错要释放半成品树，用标签把清理代码写在一处。 */
CJSON_PUBLIC(cJSON *) cJSON_ParseWithLengthOpts(const char *value, size_t buffer_length, const char **return_parse_end, cJSON_bool require_null_terminated)
{
    /* 栈上的“光标”。{0,0,0,0,{0,0,0}} 按字段顺序清零：
     * content/length/offset/depth 四个 0，内嵌 hooks 三个函数指针也是 0（NULL）。
     * content 稍后指向调用者的 JSON，我们不拥有那块内存，不能 free 它。 */
    parse_buffer buffer = { 0, 0, 0, 0, { 0, 0, 0 } };
    cJSON *item = NULL;                         /* 根结点指针，先空着；成功才指向堆上的树 */

    /* 上次失败留下的全局错误位置先清掉，免得上次的脏数据被 GetErrorPtr 读到 */
    global_error.json = NULL;                   /* 基址指针清空 */
    global_error.position = 0;                  /* 偏移清零 */

    /* || 短路：value 为 NULL 就不再看右边。没有输入或长度为 0，直接失败 */
    if (value == NULL || 0 == buffer_length)
    {
        goto fail;                              /* 跳到函数末尾统一清理；这里还没分配 item */
    }

    buffer.content = (const unsigned char*)value; /* 把调用者的 char* 当成字节序列；地址不变，只换类型标签 */
    buffer.length = buffer_length;              /* 这段输入一共多少字节（可以没有结尾 '\0'） */
    buffer.offset = 0;                          /* 光标从第 0 个字节开始 */
    buffer.hooks = global_hooks;                /* 结构体赋值：整份拷贝 malloc/free/realloc 三个函数指针 */

    item = cJSON_New_Item(&global_hooks);       /* 堆上分配根结点并 memset 成 0；& 取全局 hooks 的地址 */
    if (item == NULL)                           /* malloc 失败 */
    {
        goto fail;                              /* 还没有半成品树，fail 里 Delete 会被 if 跳过 */
    }

    /* 从内往外读这一长串：
     *   &buffer                      栈上 buffer 的地址，类型是 parse_buffer*
     *   skip_utf8_bom(...)           若开头是 EF BB BF，offset += 3；返回同一个指针
     *   buffer_skip_whitespace(...)  跳过空格/换行等，返回同一个指针
     *   parse_value(item, 那个指针)  看当前字符决定是数字/字符串/数组/对象……写入 item
     *   !                           cJSON_bool 为 0（失败）时进入 if */
    if (!parse_value(item, buffer_skip_whitespace(skip_utf8_bom(&buffer))))
    {
        goto fail;                              /* 语法错或内存不够：下面会 Delete 半成品树 */
    }

    /* require_null_terminated==1：值后面只许空白，然后必须是 '\0'，不许尾随垃圾（如 "1 2"） */
    if (require_null_terminated)
    {
        buffer_skip_whitespace(&buffer);        /* 数字/对象结束后再跳空白，光标应落在 '\0' 上 */
        /* 两种失败：光标已经越界，或当前字节不是 0。
         * buffer_at_offset(&buffer)[0] 就是 content[offset] 那个字节。 */
        if ((buffer.offset >= buffer.length) || buffer_at_offset(&buffer)[0] != '\0')
        {
            goto fail;                          /* 末尾不是干净的 C 字符串 */
        }
    }
    if (return_parse_end)                       /* 调用者传了非 NULL：想知道“解析停在哪” */
    {
        /* *p = ... 改的是调用者的那个指针变量（例如 const char *end; 传了 &end）。
         * 成功时指向值后面第一个没吃掉的字符（常是 '\0' 或尾随内容）。 */
        *return_parse_end = (const char*)buffer_at_offset(&buffer);
    }

    return item;                                /* 成功：把根结点交给调用者，对方最后要 cJSON_Delete */

fail:                                           /* 标签：上面任何 goto fail 都落到这里，相当于 C 的“统一清理” */
    if (item != NULL)                           /* 根已经分配出来了（可能带半棵树） */
    {
        cJSON_Delete(item);                     /* 递归释放 child/next 和本结点，避免泄漏 */
    }

    if (value != NULL)                          /* 有输入才能报告“错在第几个字节”；value==NULL 时没东西可指 */
    {
        error local_error;                      /* 栈上临时错误记录，最后再拷进 global_error */
        local_error.json = (const unsigned char*)value; /* 基址 = 整段 JSON 开头 */
        local_error.position = 0;               /* 先假定错在开头，下面按光标修正 */

        if (buffer.offset < buffer.length)      /* 光标还在缓冲内：错就在当前这个字节 */
        {
            local_error.position = buffer.offset; /* 例如 "12x" 会停在 'x' */
        }
        else if (buffer.length > 0)             /* 光标已经走到末尾之后：夹到最后一个合法下标 */
        {
            local_error.position = buffer.length - 1; /* length-1 才是最后一个字节，避免越界 */
        }

        if (return_parse_end != NULL)           /* 失败时也写回结束指针，位置和 GetErrorPtr 一致 */
        {
            /* json + position：指针加法，得到出错那个字节的地址，再转成 char* */
            *return_parse_end = (const char*)local_error.json + local_error.position;
        }

        global_error = local_error;             /* 结构体整体赋值；之后 cJSON_GetErrorPtr() 就能读到 */
    }

    return NULL;                                /* 约定：解析失败返回空指针，不要解引用它 */
}

/* Default options for cJSON_Parse：不要求末尾必须是 '\0' 之后立刻结束，也不返回结束指针。 */
CJSON_PUBLIC(cJSON *) cJSON_Parse(const char *value)
{
    return cJSON_ParseWithOpts(value, 0, 0);
}

CJSON_PUBLIC(cJSON *) cJSON_ParseWithLength(const char *value, size_t buffer_length)
{
    return cJSON_ParseWithLengthOpts(value, buffer_length, 0, 0);
}

#define cjson_min(a, b) (((a) < (b)) ? (a) : (b))

/* 打印的内部实现：先分配 256 字节，print_value 过程中不够就 ensure 扩容，
 * 最后把缓冲收缩/拷贝成刚好合适的、调用者需要自己 free 的字符串。 */
static unsigned char *print(const cJSON * const item, cJSON_bool format, const internal_hooks * const hooks)
{
    static const size_t default_buffer_size = 256;
    printbuffer buffer[1];
    unsigned char *printed = NULL;

    memset(buffer, 0, sizeof(buffer));

    /* create buffer */
    buffer->buffer = (unsigned char*) hooks->allocate(default_buffer_size);
    buffer->length = default_buffer_size;
    buffer->format = format;
    buffer->hooks = *hooks;
    if (buffer->buffer == NULL)
    {
        goto fail;
    }

    /* print the value */
    if (!print_value(item, buffer))
    {
        goto fail;
    }
    update_offset(buffer);

    /* check if reallocate is available */
    if (hooks->reallocate != NULL)
    {
        printed = (unsigned char*) hooks->reallocate(buffer->buffer, buffer->offset + 1);
        if (printed == NULL) {
            goto fail;
        }
        buffer->buffer = NULL;
    }
    else /* otherwise copy the JSON over to a new buffer */
    {
        printed = (unsigned char*) hooks->allocate(buffer->offset + 1);
        if (printed == NULL)
        {
            goto fail;
        }
        memcpy(printed, buffer->buffer, cjson_min(buffer->length, buffer->offset + 1));
        printed[buffer->offset] = '\0'; /* just to be sure */

        /* free the buffer */
        hooks->deallocate(buffer->buffer);
        buffer->buffer = NULL;
    }

    return printed;

fail:
    if (buffer->buffer != NULL)
    {
        hooks->deallocate(buffer->buffer);
        buffer->buffer = NULL;
    }

    if (printed != NULL)
    {
        hooks->deallocate(printed);
        printed = NULL;
    }

    return NULL;
}

/* Render a cJSON item/entity/structure to text. */
CJSON_PUBLIC(char *) cJSON_Print(const cJSON *item)
{
    return (char*)print(item, true, &global_hooks);
}

CJSON_PUBLIC(char *) cJSON_PrintUnformatted(const cJSON *item)
{
    return (char*)print(item, false, &global_hooks);
}

CJSON_PUBLIC(char *) cJSON_PrintBuffered(const cJSON *item, int prebuffer, cJSON_bool fmt)
{
    printbuffer p = { 0, 0, 0, 0, 0, 0, { 0, 0, 0 } };

    if (prebuffer < 0)
    {
        return NULL;
    }

    p.buffer = (unsigned char*)global_hooks.allocate((size_t)prebuffer);
    if (!p.buffer)
    {
        return NULL;
    }

    p.length = (size_t)prebuffer;
    p.offset = 0;
    p.noalloc = false;
    p.format = fmt;
    p.hooks = global_hooks;

    if (!print_value(item, &p))
    {
        global_hooks.deallocate(p.buffer);
        p.buffer = NULL;
        return NULL;
    }

    return (char*)p.buffer;
}

CJSON_PUBLIC(cJSON_bool) cJSON_PrintPreallocated(cJSON *item, char *buffer, const int length, const cJSON_bool format)
{
    printbuffer p = { 0, 0, 0, 0, 0, 0, { 0, 0, 0 } };

    if ((length < 0) || (buffer == NULL))
    {
        return false;
    }

    p.buffer = (unsigned char*)buffer;
    p.length = (size_t)length;
    p.offset = 0;
    p.noalloc = true;
    p.format = format;
    p.hooks = global_hooks;

    return print_value(item, &p);
}

/* 解析“一个 JSON 值”的分发函数。递归下降的核心。
 *
 * strncmp(s, "null", 4)==0  比较最多 4 个字符。不依赖 s 后面有没有 '\0'。
 * 成功后 offset += 4，光标跳过这个单词。
 *
 * 看到 '[' 就调 parse_array，它内部每个元素再调 parse_value —— 这就是递归。
 * 函数调用会在栈上再开一帧。套太多层会栈溢出，所以 array/object 里要检查 depth。 */
static cJSON_bool parse_value(cJSON * const item, parse_buffer * const input_buffer)
{
    /* 空指针防护：|| 短路，左边已经真就不再读 content，避免崩溃 */
    if ((input_buffer == NULL) || (input_buffer->content == NULL))
    {
        return false;                           /* 没有输入，解析失败 */
    }

    /* 下面按“当前光标上的字符”判断是哪种 JSON 值。先看字面量，再看 " - [ { */

    /* can_read(..., 4)：从光标起还能安全读 4 字节，才去比 "null"
     * strncmp 最多比 4 个字符，不要求后面有 '\0'（ParseWithLength 可能没有）
     * == 0 表示这 4 个字节正好是 n-u-l-l */
    if (can_read(input_buffer, 4) && (strncmp((const char*)buffer_at_offset(input_buffer), "null", 4) == 0))
    {
        item->type = cJSON_NULL;                /* 结点类型标成 null（没有 valuestring/数字） */
        input_buffer->offset += 4;              /* 光标跳过这 4 个字母，停在后面那个字符上 */
        return true;                            /* 这一个值解析成功 */
    }
    /* false 比 true/null 多一个字母，所以要能读 5 字节 */
    if (can_read(input_buffer, 5) && (strncmp((const char*)buffer_at_offset(input_buffer), "false", 5) == 0))
    {
        item->type = cJSON_False;               /* 布尔假。valueint 保持 New_Item 时的 0 */
        input_buffer->offset += 5;              /* 跳过 f-a-l-s-e */
        return true;
    }
    /* true 也是 4 个字母。注意必须先匹配 false，否则不会误伤（首字母不同） */
    if (can_read(input_buffer, 4) && (strncmp((const char*)buffer_at_offset(input_buffer), "true", 4) == 0))
    {
        item->type = cJSON_True;                /* 布尔真 */
        item->valueint = 1;                     /* 顺手填 1，方便当整数用（false 保持 0） */
        input_buffer->offset += 4;              /* 跳过 t-r-u-e */
        return true;
    }
    /* 当前字节是 '"'：这是字符串。把同一 item / 同一 buffer 交给 parse_string */
    if (can_access_at_index(input_buffer, 0) && (buffer_at_offset(input_buffer)[0] == '\"'))
    {
        return parse_string(item, input_buffer); /* 成功与否直接返回给上层 */
    }
    /* 数字：以 '-' 或 '0'～'9' 开头（JSON 不允许 "+1" 这种） */
    if (can_access_at_index(input_buffer, 0) && ((buffer_at_offset(input_buffer)[0] == '-') || ((buffer_at_offset(input_buffer)[0] >= '0') && (buffer_at_offset(input_buffer)[0] <= '9'))))
    {
        return parse_number(item, input_buffer); /* 填 valuedouble / valueint */
    }
    /* '['：数组。parse_array 会给每个元素再调 parse_value，形成递归 */
    if (can_access_at_index(input_buffer, 0) && (buffer_at_offset(input_buffer)[0] == '['))
    {
        return parse_array(item, input_buffer);
    }
    /* '{'：对象。同样递归：每个成员的值再走 parse_value */
    if (can_access_at_index(input_buffer, 0) && (buffer_at_offset(input_buffer)[0] == '{'))
    {
        return parse_object(item, input_buffer);
    }

    return false;                               /* 当前字符对不上任何 JSON 值，例如 ',' 或乱码 */
}

/* 打印分发：按 type 的低 8 位（去掉 IsReference 等标志）选择怎么写文本。 */
static cJSON_bool print_value(const cJSON * const item, printbuffer * const output_buffer)
{
    unsigned char *output = NULL;

    if ((item == NULL) || (output_buffer == NULL))
    {
        return false;
    }

    switch ((item->type) & 0xFF)
    {
        /* 0xFF = 二进制低 8 位全 1。& 之后高位的 IsReference 等被清掉，才能拿去和 case 比较。
         * switch 比较的是整数，每个 case 落到对应写法。 */
        case cJSON_NULL:
            /* "null" 4 个字符 + '\0' 共 5 字节。ensure 返回的是写入位置指针。 */
            output = ensure(output_buffer, 5);
            if (output == NULL)
            {
                return false;
            }
            strcpy((char*)output, "null");
            return true;

        case cJSON_False:
            output = ensure(output_buffer, 6);
            if (output == NULL)
            {
                return false;
            }
            strcpy((char*)output, "false");
            return true;

        case cJSON_True:
            output = ensure(output_buffer, 5);
            if (output == NULL)
            {
                return false;
            }
            strcpy((char*)output, "true");
            return true;

        case cJSON_Number:
            return print_number(item, output_buffer);

        case cJSON_Raw:
        {
            size_t raw_length = 0;
            if (item->valuestring == NULL)
            {
                return false;
            }

            raw_length = strlen(item->valuestring) + sizeof("");
            output = ensure(output_buffer, raw_length);
            if (output == NULL)
            {
                return false;
            }
            memcpy(output, item->valuestring, raw_length);
            return true;
        }

        case cJSON_String:
            return print_string(item, output_buffer);

        case cJSON_Array:
            return print_array(item, output_buffer);

        case cJSON_Object:
            return print_object(item, output_buffer);

        default:
            return false;
    }
}

/* 解析数组：[ value, value, ... ]
 *
 * 链表接法（务必在纸上画）：
 *   head         永远指向第一个元素
 *   current_item 指向当前正在解析的那个
 *   新结点：current->next = new;  new->prev = current;  current = new;
 *
 * 空数组：child 保持 NULL。
 *
 * 指针参数 item 是“数组这个结点本身”，孩子挂在 item->child 上，
 * 不是改 item 这个指针变量（所以函数返回 bool，不返回新指针）。 */
static cJSON_bool parse_array(cJSON * const item, parse_buffer * const input_buffer)
{
    cJSON *head = NULL;                         /* 第一个元素；空数组一直保持 NULL */
    cJSON *current_item = NULL;                 /* 当前刚接上、正在解析的那个元素 */

    if (input_buffer->depth >= CJSON_NESTING_LIMIT) /* 嵌套层数到顶，防止 [[[[... 撑爆调用栈 */
    {
        return false;                           /* 太深，直接失败（还没分配孩子，不用 Delete） */
    }
    input_buffer->depth++;                      /* 进入一层 '['，深度 +1；成功/失败出口都要 -- */

    if (buffer_at_offset(input_buffer)[0] != '[') /* 防御：调用方本该保证光标在 '[' 上 */
    {
        goto fail;                              /* 不是数组，走统一失败清理 */
    }

    input_buffer->offset++;                     /* 吃掉 '['，光标来到第一个元素或 ']' 前面 */
    buffer_skip_whitespace(input_buffer);       /* 跳过 "[   1" 这种括号后的空白 */
    if (can_access_at_index(input_buffer, 0) && (buffer_at_offset(input_buffer)[0] == ']'))
    {
        goto success;                           /* "[]"：没有孩子，child 保持 NULL */
    }

    /* 空白跳完后已经没字符了，例如 "[" 然后输入结束 */
    if (cannot_access_at_index(input_buffer, 0))
    {
        input_buffer->offset--;                 /* 光标退回，让错误位置落在 '[' 上，方便 GetErrorPtr */
        goto fail;
    }

    /* 故意退一格：后面 do 循环统一用 offset++ 跳过分隔符。
     * 第一次跳过的是 '['，之后每次跳过的是 ','。这样循环体不用写两套。 */
    input_buffer->offset--;
    do                                          /* do-while：至少试图读一个元素（空数组已在上面返回） */
    {
        /* 每个元素都是独立的堆结点。&hooks：传入分配器地址 */
        cJSON *new_item = cJSON_New_Item(&(input_buffer->hooks));
        if (new_item == NULL)                   /* malloc 失败 */
        {
            goto fail;                          /* fail 里会 Delete 已经接上的 head 链表 */
        }

        if (head == NULL)                       /* 还没有第一个元素 */
        {
            /* 从右往左赋值：head = new_item，再 current_item = head。
             * 三个指针指向同一块堆内存。 */
            current_item = head = new_item;
        }
        else
        {
            /* 接到尾巴：只改指针，结点不搬家。
             *  A.next → B ， B.prev → A ，然后 current 改指 B */
            current_item->next = new_item;
            new_item->prev = current_item;
            current_item = new_item;
        }

        input_buffer->offset++;                 /* 第一次跳过 '['，之后跳过 ',' */
        buffer_skip_whitespace(input_buffer);   /* 逗号后面也可能有空格：",  2" */
        if (!parse_value(current_item, input_buffer)) /* 递归：元素可以是数字/字符串/再套一层数组 */
        {
            goto fail;                          /* 这个元素解析失败，整段数组作废 */
        }
        buffer_skip_whitespace(input_buffer);   /* 值后面的空白，好判断下一个字符是 ',' 还是 ']' */
    }
    /* 当前字符是逗号就再转一圈；否则结束循环（期望看到 ']'） */
    while (can_access_at_index(input_buffer, 0) && (buffer_at_offset(input_buffer)[0] == ','));

    if (cannot_access_at_index(input_buffer, 0) || buffer_at_offset(input_buffer)[0] != ']')
    {
        goto fail;                              /* 缺 ']'，或 [1 2] 这种少逗号 */
    }

success:                                        /* 空数组和有元素的数组都到这里收尾 */
    input_buffer->depth--;                      /* 离开这一层 '['，和上面的 ++ 配对 */

    if (head != NULL) {                         /* 非空：做一个“首尾相接”的小优化 */
        /* 第一个结点的 prev 指向最后一个。以后往数组尾追加时，用 child->prev 就能找到尾巴，
         * 不用从 head 一路 next 走过去。这不是完整的循环链表（尾巴的 next 仍是 NULL）。 */
        head->prev = current_item;
    }

    item->type = cJSON_Array;                   /* 这个结点（原来是空壳）现在是数组 */
    item->child = head;                         /* 孩子链表挂上去；空数组则 child == NULL */

    input_buffer->offset++;                     /* 吃掉 ']'，光标停在数组后面 */

    return true;                                /* 成功；失败路径不会走到这里 */

fail:
    if (head != NULL)                           /* 已经接出若干元素 */
    {
        cJSON_Delete(head);                     /* 整条孩子链表释放，避免泄漏半成品 */
    }

    return false;                               /* 调用方（parse_value）会再 Delete 数组结点自己 */
}

/* 把数组打印成 [a, b, c]。format 时逗号后加空格。沿 child->next 走，每个元素调 print_value。 */
static cJSON_bool print_array(const cJSON * const item, printbuffer * const output_buffer)
{
    unsigned char *output_pointer = NULL;
    size_t length = 0;
    cJSON *current_element = item->child;

    if (output_buffer == NULL)
    {
        return false;
    }

    if (output_buffer->depth >= CJSON_NESTING_LIMIT)
    {
        return false; /* nesting is too deep */
    }

    /* Compose the output array. */
    /* opening square bracket */
    output_pointer = ensure(output_buffer, 1);
    if (output_pointer == NULL)
    {
        return false;
    }

    *output_pointer = '[';
    output_buffer->offset++;
    output_buffer->depth++;

    /* 沿兄弟指针走，不是用下标。C 数组才是 a[i]，链表只能 p = p->next。 */
    while (current_element != NULL)
    {
        if (!print_value(current_element, output_buffer))
        {
            return false;
        }
        update_offset(output_buffer);
        if (current_element->next)
        {
            length = (size_t) (output_buffer->format ? 2 : 1);
            output_pointer = ensure(output_buffer, length + 1);
            if (output_pointer == NULL)
            {
                return false;
            }
            *output_pointer++ = ',';
            if(output_buffer->format)
            {
                *output_pointer++ = ' ';
            }
            /* 先写内容再写 '\0'。++ 在赋值之后让指针指向下一格。 */
            *output_pointer = '\0';
            output_buffer->offset += length;
        }
        current_element = current_element->next;
    }

    output_pointer = ensure(output_buffer, 2);
    if (output_pointer == NULL)
    {
        return false;
    }
    *output_pointer++ = ']';
    *output_pointer = '\0';
    output_buffer->depth--;

    return true;
}

/* 解析对象：{ "key": value, ... }
 * 和数组几乎一样，多两步：
 *   1. 先 parse_string 读键名；parse_string 会写到 valuestring，再“挪”到 string 字段。
 *   2. 必须看到 ':'，再 parse_value 读真正的值。 */
static cJSON_bool parse_object(cJSON * const item, parse_buffer * const input_buffer)
{
    cJSON *head = NULL;                         /* 第一个成员；空对象一直保持 NULL */
    cJSON *current_item = NULL;                 /* 当前正在解析的那个键值对结点 */

    if (input_buffer->depth >= CJSON_NESTING_LIMIT) /* 和数组一样：嵌套太深会撑爆栈 */
    {
        return false;                           /* 还没分配孩子，直接返回 */
    }
    input_buffer->depth++;                      /* 进入一层 '{' */

    /* 比数组多一次越界检查：光标必须还能读，且当前是 '{' */
    if (cannot_access_at_index(input_buffer, 0) || (buffer_at_offset(input_buffer)[0] != '{'))
    {
        goto fail;                              /* 不是对象 */
    }

    input_buffer->offset++;                     /* 吃掉 '{' */
    buffer_skip_whitespace(input_buffer);       /* 跳过 "{  \"a\"" 这种空白 */
    if (can_access_at_index(input_buffer, 0) && (buffer_at_offset(input_buffer)[0] == '}'))
    {
        goto success;                           /* "{}"：没有成员 */
    }

    if (cannot_access_at_index(input_buffer, 0)) /* "{" 之后输入就没了 */
    {
        input_buffer->offset--;                 /* 错误位置退回 '{' */
        goto fail;
    }

    /* 和数组同一手法：退一格，循环里统一 offset++ 跳过 '{' 或 ',' */
    input_buffer->offset--;
    do                                          /* 至少读一对 "key": value */
    {
        cJSON *new_item = cJSON_New_Item(&(input_buffer->hooks)); /* 一个成员 = 一个 cJSON 结点 */
        if (new_item == NULL)
        {
            goto fail;                          /* 分配失败，释放已接上的成员 */
        }

        if (head == NULL)                       /* 第一个成员 */
        {
            current_item = head = new_item;     /* 三个指针指向同一块 */
        }
        else
        {
            current_item->next = new_item;      /* 接到链表尾巴 */
            new_item->prev = current_item;
            current_item = new_item;
        }

        if (cannot_access_at_index(input_buffer, 1)) /* 逗号/括号后面至少还要有一个字节（键名） */
        {
            goto fail;                          /* "{," 或末尾截断 */
        }

        input_buffer->offset++;                 /* 第一次跳 '{'，之后跳 ',' */
        buffer_skip_whitespace(input_buffer);   /* 来到键名左边的 '"' */
        if (!parse_string(current_item, input_buffer)) /* 键必须是字符串；写进 valuestring */
        {
            goto fail;                          /* 键不是 "..."，例如 {1:2} */
        }
        buffer_skip_whitespace(input_buffer);   /* "key" 和 ':' 之间的空白 */

        /* parse_string 只知道往 valuestring 写。对象规定键名放在 string 字段。
         * 这是指针赋值，不是拷贝字符串：两指针曾指向同一块堆内存。
         * 必须立刻把 valuestring 置 NULL，否则 Delete 会 free 两次（double free）。 */
        current_item->string = current_item->valuestring;
        current_item->valuestring = NULL;

        if (cannot_access_at_index(input_buffer, 0) || (buffer_at_offset(input_buffer)[0] != ':'))
        {
            goto fail;                          /* 键后面必须是冒号，{"a" 1} 非法 */
        }

        input_buffer->offset++;                 /* 吃掉 ':' */
        buffer_skip_whitespace(input_buffer);   /* 来到值的第一个字符 */
        if (!parse_value(current_item, input_buffer)) /* 递归解析值（可再套对象/数组） */
        {
            goto fail;                          /* 值解析失败 */
        }
        buffer_skip_whitespace(input_buffer);   /* 值后面，准备看 ',' 还是 '}' */
    }
    while (can_access_at_index(input_buffer, 0) && (buffer_at_offset(input_buffer)[0] == ','));

    if (cannot_access_at_index(input_buffer, 0) || (buffer_at_offset(input_buffer)[0] != '}'))
    {
        goto fail;                              /* 缺 '}'，或两个成员之间少逗号 */
    }

success:
    input_buffer->depth--;                      /* 离开这一层 '{' */

    if (head != NULL) {                         /* 和非空数组相同：首结点 prev 记住尾巴 */
        head->prev = current_item;
    }

    item->type = cJSON_Object;                  /* 这个结点现在是对象 */
    item->child = head;                         /* 成员链表挂到 child 上 */

    input_buffer->offset++;                     /* 吃掉 '}' */
    return true;

fail:
    if (head != NULL)
    {
        cJSON_Delete(head);                     /* 已解析的成员（含键名字符串）全部释放 */
    }

    return false;
}

/* 把对象打印成 { "k": v, ... }。format 时每个键前打 depth 个 Tab，键值之间有换行。 */
static cJSON_bool print_object(const cJSON * const item, printbuffer * const output_buffer)
{
    unsigned char *output_pointer = NULL;
    size_t length = 0;
    cJSON *current_item = item->child;

    if (output_buffer == NULL)
    {
        return false;
    }

    if (output_buffer->depth >= CJSON_NESTING_LIMIT)
    {
        return false; /* nesting is too deep */
    }

    /* Compose the output: */
    length = (size_t) (output_buffer->format ? 2 : 1); /* fmt: {\n */
    output_pointer = ensure(output_buffer, length + 1);
    if (output_pointer == NULL)
    {
        return false;
    }

    *output_pointer++ = '{';
    output_buffer->depth++;
    if (output_buffer->format)
    {
        *output_pointer++ = '\n';
    }
    output_buffer->offset += length;

    while (current_item)
    {
        if (output_buffer->format)
        {
            size_t i;
            output_pointer = ensure(output_buffer, output_buffer->depth);
            if (output_pointer == NULL)
            {
                return false;
            }
            for (i = 0; i < output_buffer->depth; i++)
            {
                *output_pointer++ = '\t';
            }
            output_buffer->offset += output_buffer->depth;
        }

        /* print key */
        if (!print_string_ptr((unsigned char*)current_item->string, output_buffer))
        {
            return false;
        }
        update_offset(output_buffer);

        length = (size_t) (output_buffer->format ? 2 : 1);
        output_pointer = ensure(output_buffer, length);
        if (output_pointer == NULL)
        {
            return false;
        }
        *output_pointer++ = ':';
        if (output_buffer->format)
        {
            *output_pointer++ = '\t';
        }
        output_buffer->offset += length;

        /* print value */
        if (!print_value(current_item, output_buffer))
        {
            return false;
        }
        update_offset(output_buffer);

        /* print comma if not last */
        length = ((size_t)(output_buffer->format ? 1 : 0) + (size_t)(current_item->next ? 1 : 0));
        output_pointer = ensure(output_buffer, length + 1);
        if (output_pointer == NULL)
        {
            return false;
        }
        if (current_item->next)
        {
            *output_pointer++ = ',';
        }

        if (output_buffer->format)
        {
            *output_pointer++ = '\n';
        }
        *output_pointer = '\0';
        output_buffer->offset += length;

        current_item = current_item->next;
    }

    output_pointer = ensure(output_buffer, output_buffer->format ? (output_buffer->depth + 1) : 2);
    if (output_pointer == NULL)
    {
        return false;
    }
    if (output_buffer->format)
    {
        size_t i;
        for (i = 0; i < (output_buffer->depth - 1); i++)
        {
            *output_pointer++ = '\t';
        }
    }
    *output_pointer++ = '}';
    *output_pointer = '\0';
    output_buffer->depth--;

    return true;
}

/* Get Array size/item / object item.
 * 数组和对象在内存里都是 child 链表，所以“数有几个元素”就是沿着 next 走一遍。
 * 注意：这是 O(n)，不是 O(1)。 */
CJSON_PUBLIC(int) cJSON_GetArraySize(const cJSON *array)
{
    cJSON *child = NULL;
    size_t size = 0;

    if (array == NULL)
    {
        return 0;
    }

    child = array->child;

    while(child != NULL)
    {
        size++;
        child = child->next;
    }

    /* FIXME: Can overflow here. Cannot be fixed without breaking the API */

    return (int)size;
}

/* 按下标取数组元素。没有 O(1) 的 a[i]，只能从头走 i 步。
 * index-- 直到 0；中途 current_child 变成 NULL 说明越界，返回 NULL。 */
static cJSON* get_array_item(const cJSON *array, size_t index)
{
    cJSON *current_child = NULL;

    if (array == NULL)
    {
        return NULL;
    }

    current_child = array->child;
    while ((current_child != NULL) && (index > 0))
    {
        index--;
        current_child = current_child->next;
    }

    return current_child;
}

CJSON_PUBLIC(cJSON *) cJSON_GetArrayItem(const cJSON *array, int index)
{
    if (index < 0)
    {
        return NULL;
    }

    return get_array_item(array, (size_t)index);
}

/* 在对象的孩子链表里按键名查找。默认大小写不敏感（JSON 标准其实是敏感的，这是历史兼容）。 */
static cJSON *get_object_item(const cJSON * const object, const char * const name, const cJSON_bool case_sensitive)
{
    cJSON *current_element = NULL;

    if ((object == NULL) || (name == NULL))
    {
        return NULL;
    }

    current_element = object->child;
    if (case_sensitive)
    {
        while ((current_element != NULL) && (current_element->string != NULL) && (strcmp(name, current_element->string) != 0))
        {
            current_element = current_element->next;
        }
    }
    else
    {
        while ((current_element != NULL) && (case_insensitive_strcmp((const unsigned char*)name, (const unsigned char*)(current_element->string)) != 0))
        {
            current_element = current_element->next;
        }
    }

    if ((current_element == NULL) || (current_element->string == NULL)) {
        return NULL;
    }

    return current_element;
}

CJSON_PUBLIC(cJSON *) cJSON_GetObjectItem(const cJSON * const object, const char * const string)
{
    return get_object_item(object, string, false);
}

CJSON_PUBLIC(cJSON *) cJSON_GetObjectItemCaseSensitive(const cJSON * const object, const char * const string)
{
    return get_object_item(object, string, true);
}

CJSON_PUBLIC(cJSON_bool) cJSON_HasObjectItem(const cJSON *object, const char *string)
{
    return cJSON_GetObjectItem(object, string) ? 1 : 0;
}

/* 把 prev 和 item 接成双向链表的相邻结点。 */
static void suffix_object(cJSON *prev, cJSON *item)
{
    prev->next = item;
    item->prev = prev;
}

/* 浅拷贝：memcpy 整颗结构体（所有指针字段的“地址值”被复制，不是深拷贝所指内容）。
 * 然后 type |= IsReference：Delete 这个新结点时不要 free 那些借来的指针。
 * next/prev 清掉，避免新结点和原链表串在一起。 */
static cJSON *create_reference(const cJSON *item, const internal_hooks * const hooks)
{
    cJSON *reference = NULL;
    if (item == NULL)
    {
        return NULL;
    }

    reference = cJSON_New_Item(hooks);
    if (reference == NULL)
    {
        return NULL;
    }

    memcpy(reference, item, sizeof(cJSON));
    reference->string = NULL;
    reference->type |= cJSON_IsReference;
    reference->next = reference->prev = NULL;
    return reference;
}

/* 把 item 接到 array（或 object）的孩子链表末尾。
 *
 * 数组/对象的元素都挂在 array->child 上，用 next/prev 串成双向链表。
 * 为了追加是 O(1) 而不是从头走到尾：
 *   第一个孩子的 prev 永远指向当前最后一个孩子（解析时也是这么记的）。
 * 空链表时用 item->prev = item（自己指自己）表示“它既是头也是尾”。 */
static cJSON_bool add_item_to_array(cJSON *array, cJSON *item)
{
    cJSON *child = NULL;                        /* 稍后等于现在的第一个孩子，可能是 NULL */

    /* 空指针不能接；array == item 是自己把自己当孩子，会成环，直接拒绝 */
    if ((item == NULL) || (array == NULL) || (array == item))
    {
        return false;
    }

    child = array->child;                       /* 记下当前头结点；后面只读这个指针，不再反复写 array->child */
    if (child == NULL)                          /* 还没有任何元素：[] 或 {} */
    {
        array->child = item;                    /* 头就是这个新结点 */
        item->prev = item;                      /* 自己指自己：它现在也是尾巴。下次追加用 child->prev 就能找到它 */
        item->next = NULL;                      /* 后面没有兄弟；遍历靠 next==NULL 结束，不是循环链表 */
    }
    else                                        /* 已经有至少一个孩子 */
    {
        if (child->prev)                        /* 正常情况下头的 prev 一定指向当前尾巴（含“只有一个、prev 指自己”） */
        {
            /* suffix_object(尾巴, item)：尾巴.next → item，item.prev → 尾巴。只改这两个指针。 */
            suffix_object(child->prev, item);
            array->child->prev = item;          /* 头的 prev 改指新尾巴，保持“prev 就是末尾”这个约定 */
        }
    }

    return true;                                /* 约定成功返回非 0。注意：item 的所有权交给 array，之后由 Delete(array) 一起释放 */
}

/* Add item to array/object. */
CJSON_PUBLIC(cJSON_bool) cJSON_AddItemToArray(cJSON *array, cJSON *item)
{
    return add_item_to_array(array, item);
}

#if defined(__clang__) || (defined(__GNUC__)  && ((__GNUC__ > 4) || ((__GNUC__ == 4) && (__GNUC_MINOR__ > 5))))
    #pragma GCC diagnostic push
#endif
#ifdef __GNUC__
#pragma GCC diagnostic ignored "-Wcast-qual"
#endif
/* helper function to cast away const */
static void* cast_away_const(const void* string)
{
    return (void*)string;
}
#if defined(__clang__) || (defined(__GNUC__)  && ((__GNUC__ > 4) || ((__GNUC__ == 4) && (__GNUC_MINOR__ > 5))))
    #pragma GCC diagnostic pop
#endif


/* 对象 = 带键名的数组。先给 item->string 赋上键名，再当数组成员追加。
 *
 * constant_key=1：new_key 直接等于传入的指针（字面量 "age" 在只读段），
 *   打上 StringIsConst，Delete 时不 free。这叫“借用”，不是“拥有”。
 * constant_key=0：strdup 一份到堆上，结点拥有这块内存。
 *
 * ~cJSON_StringIsConst  按位取反后再 &，用来清掉这一位。 */
static cJSON_bool add_item_to_object(cJSON * const object, const char * const string, cJSON * const item, const internal_hooks * const hooks, const cJSON_bool constant_key)
{
    char *new_key = NULL;
    int new_type = cJSON_Invalid;

    if ((object == NULL) || (string == NULL) || (item == NULL) || (object == item))
    {
        return false;
    }

    if (constant_key)
    {
        new_key = (char*)cast_away_const(string);
        new_type = item->type | cJSON_StringIsConst;
    }
    else
    {
        new_key = (char*)cJSON_strdup((const unsigned char*)string, hooks);
        if (new_key == NULL)
        {
            return false;
        }

        new_type = item->type & ~cJSON_StringIsConst;
    }

    if (!(item->type & cJSON_StringIsConst) && (item->string != NULL))
    {
        hooks->deallocate(item->string);
    }

    item->string = new_key;
    item->type = new_type;

    return add_item_to_array(object, item);
}

CJSON_PUBLIC(cJSON_bool) cJSON_AddItemToObject(cJSON *object, const char *string, cJSON *item)
{
    return add_item_to_object(object, string, item, &global_hooks, false);
}

/* Add an item to an object with constant string as key */
CJSON_PUBLIC(cJSON_bool) cJSON_AddItemToObjectCS(cJSON *object, const char *string, cJSON *item)
{
    return add_item_to_object(object, string, item, &global_hooks, true);
}

CJSON_PUBLIC(cJSON_bool) cJSON_AddItemReferenceToArray(cJSON *array, cJSON *item)
{
    if (array == NULL)
    {
        return false;
    }

    return add_item_to_array(array, create_reference(item, &global_hooks));
}

CJSON_PUBLIC(cJSON_bool) cJSON_AddItemReferenceToObject(cJSON *object, const char *string, cJSON *item)
{
    if ((object == NULL) || (string == NULL))
    {
        return false;
    }

    return add_item_to_object(object, string, create_reference(item, &global_hooks), &global_hooks, false);
}

CJSON_PUBLIC(cJSON*) cJSON_AddNullToObject(cJSON * const object, const char * const name)
{
    cJSON *null = cJSON_CreateNull();
    if (add_item_to_object(object, name, null, &global_hooks, false))
    {
        return null;
    }

    cJSON_Delete(null);
    return NULL;
}

CJSON_PUBLIC(cJSON*) cJSON_AddTrueToObject(cJSON * const object, const char * const name)
{
    cJSON *true_item = cJSON_CreateTrue();
    if (add_item_to_object(object, name, true_item, &global_hooks, false))
    {
        return true_item;
    }

    cJSON_Delete(true_item);
    return NULL;
}

CJSON_PUBLIC(cJSON*) cJSON_AddFalseToObject(cJSON * const object, const char * const name)
{
    cJSON *false_item = cJSON_CreateFalse();
    if (add_item_to_object(object, name, false_item, &global_hooks, false))
    {
        return false_item;
    }

    cJSON_Delete(false_item);
    return NULL;
}

CJSON_PUBLIC(cJSON*) cJSON_AddBoolToObject(cJSON * const object, const char * const name, const cJSON_bool boolean)
{
    cJSON *bool_item = cJSON_CreateBool(boolean);
    if (add_item_to_object(object, name, bool_item, &global_hooks, false))
    {
        return bool_item;
    }

    cJSON_Delete(bool_item);
    return NULL;
}

CJSON_PUBLIC(cJSON*) cJSON_AddNumberToObject(cJSON * const object, const char * const name, const double number)
{
    cJSON *number_item = cJSON_CreateNumber(number);
    if (add_item_to_object(object, name, number_item, &global_hooks, false))
    {
        return number_item;
    }

    cJSON_Delete(number_item);
    return NULL;
}

CJSON_PUBLIC(cJSON*) cJSON_AddStringToObject(cJSON * const object, const char * const name, const char * const string)
{
    cJSON *string_item = cJSON_CreateString(string);
    if (add_item_to_object(object, name, string_item, &global_hooks, false))
    {
        return string_item;
    }

    cJSON_Delete(string_item);
    return NULL;
}

CJSON_PUBLIC(cJSON*) cJSON_AddRawToObject(cJSON * const object, const char * const name, const char * const raw)
{
    cJSON *raw_item = cJSON_CreateRaw(raw);
    if (add_item_to_object(object, name, raw_item, &global_hooks, false))
    {
        return raw_item;
    }

    cJSON_Delete(raw_item);
    return NULL;
}

CJSON_PUBLIC(cJSON*) cJSON_AddObjectToObject(cJSON * const object, const char * const name)
{
    cJSON *object_item = cJSON_CreateObject();
    if (add_item_to_object(object, name, object_item, &global_hooks, false))
    {
        return object_item;
    }

    cJSON_Delete(object_item);
    return NULL;
}

CJSON_PUBLIC(cJSON*) cJSON_AddArrayToObject(cJSON * const object, const char * const name)
{
    cJSON *array = cJSON_CreateArray();
    if (add_item_to_object(object, name, array, &global_hooks, false))
    {
        return array;
    }

    cJSON_Delete(array);
    return NULL;
}

/* 从父结点的孩子链表里摘下 item，不释放内存。
 *
 * 双向链表删除的四句话（中间结点）：
 *   item->prev->next = item->next;  左边那个改“后继”
 *   item->next->prev = item->prev;  右边那个改“前驱”
 *   item->prev = NULL;
 *   item->next = NULL;              摘下来的结点不再指向原来的邻居
 *
 * 还要处理：删的是头（parent->child 改成第二个）、删的是尾（头的 prev 改成新尾）。
 * 返回的指针仍有效，调用者可以 Delete，或 Add 到别处。 */
CJSON_PUBLIC(cJSON *) cJSON_DetachItemViaPointer(cJSON *parent, cJSON * const item)
{
    if ((parent == NULL) || (item == NULL) || (item != parent->child && item->prev == NULL))
    {
        return NULL;
    }

    if (item != parent->child)
    {
        /* not the first element */
        item->prev->next = item->next;
    }
    if (item->next != NULL)
    {
        /* not the last element */
        item->next->prev = item->prev;
    }

    if (item == parent->child)
    {
        /* first element */
        parent->child = item->next;
    }
    else if (item->next == NULL)
    {
        /* last element */
        parent->child->prev = item->prev;
    }

    /* make sure the detached item doesn't point anywhere anymore */
    item->prev = NULL;
    item->next = NULL;

    return item;
}

CJSON_PUBLIC(cJSON *) cJSON_DetachItemFromArray(cJSON *array, int which)
{
    if (which < 0)
    {
        return NULL;
    }

    return cJSON_DetachItemViaPointer(array, get_array_item(array, (size_t)which));
}

CJSON_PUBLIC(void) cJSON_DeleteItemFromArray(cJSON *array, int which)
{
    cJSON_Delete(cJSON_DetachItemFromArray(array, which));
}

CJSON_PUBLIC(cJSON *) cJSON_DetachItemFromObject(cJSON *object, const char *string)
{
    cJSON *to_detach = cJSON_GetObjectItem(object, string);

    return cJSON_DetachItemViaPointer(object, to_detach);
}

CJSON_PUBLIC(cJSON *) cJSON_DetachItemFromObjectCaseSensitive(cJSON *object, const char *string)
{
    cJSON *to_detach = cJSON_GetObjectItemCaseSensitive(object, string);

    return cJSON_DetachItemViaPointer(object, to_detach);
}

CJSON_PUBLIC(void) cJSON_DeleteItemFromObject(cJSON *object, const char *string)
{
    cJSON_Delete(cJSON_DetachItemFromObject(object, string));
}

CJSON_PUBLIC(void) cJSON_DeleteItemFromObjectCaseSensitive(cJSON *object, const char *string)
{
    cJSON_Delete(cJSON_DetachItemFromObjectCaseSensitive(object, string));
}

/* Replace array/object items with new ones. */
CJSON_PUBLIC(cJSON_bool) cJSON_InsertItemInArray(cJSON *array, int which, cJSON *newitem)
{
    cJSON *after_inserted = NULL;

    if (which < 0 || newitem == NULL)
    {
        return false;
    }

    after_inserted = get_array_item(array, (size_t)which);
    if (after_inserted == NULL)
    {
        return add_item_to_array(array, newitem);
    }

    if (after_inserted != array->child && after_inserted->prev == NULL) {
        /* return false if after_inserted is a corrupted array item */
        return false;
    }

    newitem->next = after_inserted;
    newitem->prev = after_inserted->prev;
    after_inserted->prev = newitem;
    if (after_inserted == array->child)
    {
        array->child = newitem;
    }
    else
    {
        newitem->prev->next = newitem;
    }
    return true;
}

CJSON_PUBLIC(cJSON_bool) cJSON_ReplaceItemViaPointer(cJSON * const parent, cJSON * const item, cJSON * replacement)
{
    if ((parent == NULL) || (parent->child == NULL) || (replacement == NULL) || (item == NULL))
    {
        return false;
    }

    if (replacement == item)
    {
        return true;
    }

    replacement->next = item->next;
    replacement->prev = item->prev;

    if (replacement->next != NULL)
    {
        replacement->next->prev = replacement;
    }
    if (parent->child == item)
    {
        if (parent->child->prev == parent->child)
        {
            replacement->prev = replacement;
        }
        parent->child = replacement;
    }
    else
    {   /*
         * To find the last item in array quickly, we use prev in array.
         * We can't modify the last item's next pointer where this item was the parent's child
         */
        if (replacement->prev != NULL)
        {
            replacement->prev->next = replacement;
        }
        if (replacement->next == NULL)
        {
            parent->child->prev = replacement;
        }
    }

    item->next = NULL;
    item->prev = NULL;
    cJSON_Delete(item);

    return true;
}

CJSON_PUBLIC(cJSON_bool) cJSON_ReplaceItemInArray(cJSON *array, int which, cJSON *newitem)
{
    if (which < 0)
    {
        return false;
    }

    return cJSON_ReplaceItemViaPointer(array, get_array_item(array, (size_t)which), newitem);
}

static cJSON_bool replace_item_in_object(cJSON *object, const char *string, cJSON *replacement, cJSON_bool case_sensitive)
{
    if ((replacement == NULL) || (string == NULL))
    {
        return false;
    }

    /* replace the name in the replacement */
    if (!(replacement->type & cJSON_StringIsConst) && (replacement->string != NULL))
    {
        cJSON_free(replacement->string);
    }
    replacement->string = (char*)cJSON_strdup((const unsigned char*)string, &global_hooks);
    if (replacement->string == NULL)
    {
        return false;
    }

    replacement->type &= ~cJSON_StringIsConst;

    return cJSON_ReplaceItemViaPointer(object, get_object_item(object, string, case_sensitive), replacement);
}

CJSON_PUBLIC(cJSON_bool) cJSON_ReplaceItemInObject(cJSON *object, const char *string, cJSON *newitem)
{
    return replace_item_in_object(object, string, newitem, false);
}

CJSON_PUBLIC(cJSON_bool) cJSON_ReplaceItemInObjectCaseSensitive(cJSON *object, const char *string, cJSON *newitem)
{
    return replace_item_in_object(object, string, newitem, true);
}

/* Create basic types: 手工造树时用这些。Parse 走的是另一条路（parse_* 填字段）。
 * CreateString 会 strdup，所以原来的 C 字符串你可以立刻释放。 */
CJSON_PUBLIC(cJSON *) cJSON_CreateNull(void)
{
    cJSON *item = cJSON_New_Item(&global_hooks);
    if(item)
    {
        item->type = cJSON_NULL;
    }

    return item;
}

CJSON_PUBLIC(cJSON *) cJSON_CreateTrue(void)
{
    cJSON *item = cJSON_New_Item(&global_hooks);
    if(item)
    {
        item->type = cJSON_True;
    }

    return item;
}

CJSON_PUBLIC(cJSON *) cJSON_CreateFalse(void)
{
    cJSON *item = cJSON_New_Item(&global_hooks);
    if(item)
    {
        item->type = cJSON_False;
    }

    return item;
}

CJSON_PUBLIC(cJSON *) cJSON_CreateBool(cJSON_bool boolean)
{
    cJSON *item = cJSON_New_Item(&global_hooks);
    if(item)
    {
        item->type = boolean ? cJSON_True : cJSON_False;
    }

    return item;
}

/* 手工造一个数字结点。和 Parse 不同：不读文本，直接填字段。
 * (int)num 是强制转换，小数部分截断。超出 int 范围不能直接转，见 saturation。 */
CJSON_PUBLIC(cJSON *) cJSON_CreateNumber(double num)
{
    cJSON *item = cJSON_New_Item(&global_hooks);
    if(item)
    {
        item->type = cJSON_Number;
        item->valuedouble = num;

        /* use saturation in case of overflow */
        if (num >= INT_MAX)
        {
            item->valueint = INT_MAX;
        }
        else if (num <= (double)INT_MIN)
        {
            item->valueint = INT_MIN;
        }
        else
        {
            item->valueint = (int)num;
        }
    }

    return item;
}

/* strdup 之后 item 拥有自己的那份字符。参数 string 可以是字面量或马上要释放的缓冲。
 * 若 strdup 失败：已经 New_Item 成功，必须 Delete(item)，否则泄漏这个空结点。 */
CJSON_PUBLIC(cJSON *) cJSON_CreateString(const char *string)
{
    cJSON *item = cJSON_New_Item(&global_hooks);
    if(item)
    {
        item->type = cJSON_String;
        item->valuestring = (char*)cJSON_strdup((const unsigned char*)string, &global_hooks);
        if(!item->valuestring)
        {
            cJSON_Delete(item);
            return NULL;
        }
    }

    return item;
}

CJSON_PUBLIC(cJSON *) cJSON_CreateStringReference(const char *string)
{
    cJSON *item = cJSON_New_Item(&global_hooks);
    if (item != NULL)
    {
        item->type = cJSON_String | cJSON_IsReference;
        item->valuestring = (char*)cast_away_const(string);
    }

    return item;
}

CJSON_PUBLIC(cJSON *) cJSON_CreateObjectReference(const cJSON *child)
{
    cJSON *item = cJSON_New_Item(&global_hooks);
    if (item != NULL) {
        item->type = cJSON_Object | cJSON_IsReference;
        item->child = (cJSON*)cast_away_const(child);
    }

    return item;
}

CJSON_PUBLIC(cJSON *) cJSON_CreateArrayReference(const cJSON *child) {
    cJSON *item = cJSON_New_Item(&global_hooks);
    if (item != NULL) {
        item->type = cJSON_Array | cJSON_IsReference;
        item->child = (cJSON*)cast_away_const(child);
    }

    return item;
}

CJSON_PUBLIC(cJSON *) cJSON_CreateRaw(const char *raw)
{
    cJSON *item = cJSON_New_Item(&global_hooks);
    if(item)
    {
        item->type = cJSON_Raw;
        item->valuestring = (char*)cJSON_strdup((const unsigned char*)raw, &global_hooks);
        if(!item->valuestring)
        {
            cJSON_Delete(item);
            return NULL;
        }
    }

    return item;
}

CJSON_PUBLIC(cJSON *) cJSON_CreateArray(void)
{
    cJSON *item = cJSON_New_Item(&global_hooks);
    if(item)
    {
        item->type=cJSON_Array;
    }

    return item;
}

CJSON_PUBLIC(cJSON *) cJSON_CreateObject(void)
{
    cJSON *item = cJSON_New_Item(&global_hooks);
    if (item)
    {
        item->type = cJSON_Object;
    }

    return item;
}

/* Create Arrays: */
CJSON_PUBLIC(cJSON *) cJSON_CreateIntArray(const int *numbers, int count)
{
    size_t i = 0;
    cJSON *n = NULL;
    cJSON *p = NULL;
    cJSON *a = NULL;

    if ((count < 0) || (numbers == NULL))
    {
        return NULL;
    }

    a = cJSON_CreateArray();

    for(i = 0; a && (i < (size_t)count); i++)
    {
        n = cJSON_CreateNumber(numbers[i]);
        if (!n)
        {
            cJSON_Delete(a);
            return NULL;
        }
        if(!i)
        {
            a->child = n;
        }
        else
        {
            suffix_object(p, n);
        }
        p = n;
    }

    if (a && a->child) {
        a->child->prev = n;
    }

    return a;
}

CJSON_PUBLIC(cJSON *) cJSON_CreateFloatArray(const float *numbers, int count)
{
    size_t i = 0;
    cJSON *n = NULL;
    cJSON *p = NULL;
    cJSON *a = NULL;

    if ((count < 0) || (numbers == NULL))
    {
        return NULL;
    }

    a = cJSON_CreateArray();

    for(i = 0; a && (i < (size_t)count); i++)
    {
        n = cJSON_CreateNumber((double)numbers[i]);
        if(!n)
        {
            cJSON_Delete(a);
            return NULL;
        }
        if(!i)
        {
            a->child = n;
        }
        else
        {
            suffix_object(p, n);
        }
        p = n;
    }

    if (a && a->child) {
        a->child->prev = n;
    }

    return a;
}

CJSON_PUBLIC(cJSON *) cJSON_CreateDoubleArray(const double *numbers, int count)
{
    size_t i = 0;
    cJSON *n = NULL;
    cJSON *p = NULL;
    cJSON *a = NULL;

    if ((count < 0) || (numbers == NULL))
    {
        return NULL;
    }

    a = cJSON_CreateArray();

    for(i = 0; a && (i < (size_t)count); i++)
    {
        n = cJSON_CreateNumber(numbers[i]);
        if(!n)
        {
            cJSON_Delete(a);
            return NULL;
        }
        if(!i)
        {
            a->child = n;
        }
        else
        {
            suffix_object(p, n);
        }
        p = n;
    }

    if (a && a->child) {
        a->child->prev = n;
    }

    return a;
}

CJSON_PUBLIC(cJSON *) cJSON_CreateStringArray(const char *const *strings, int count)
{
    size_t i = 0;
    cJSON *n = NULL;
    cJSON *p = NULL;
    cJSON *a = NULL;

    if ((count < 0) || (strings == NULL))
    {
        return NULL;
    }

    a = cJSON_CreateArray();

    for (i = 0; a && (i < (size_t)count); i++)
    {
        n = cJSON_CreateString(strings[i]);
        if(!n)
        {
            cJSON_Delete(a);
            return NULL;
        }
        if(!i)
        {
            a->child = n;
        }
        else
        {
            suffix_object(p,n);
        }
        p = n;
    }

    if (a && a->child) {
        a->child->prev = n;
    }

    return a;
}

/* Duplication */
cJSON * cJSON_Duplicate_rec(const cJSON *item, size_t depth, cJSON_bool recurse);

/* 深拷贝（recurse=1）或只拷当前结点（recurse=0）。新树的 next/prev 会重新接好，
 * 和原树完全独立，两边可以分别 Delete。 */
CJSON_PUBLIC(cJSON *) cJSON_Duplicate(const cJSON *item, cJSON_bool recurse)
{
    return cJSON_Duplicate_rec(item, 0, recurse );
}

cJSON * cJSON_Duplicate_rec(const cJSON *item, size_t depth, cJSON_bool recurse)
{
    cJSON *newitem = NULL;
    cJSON *child = NULL;
    cJSON *next = NULL;
    cJSON *newchild = NULL;

    /* Bail on bad ptr */
    if (!item)
    {
        goto fail;
    }
    /* Create new item */
    newitem = cJSON_New_Item(&global_hooks);
    if (!newitem)
    {
        goto fail;
    }
    /* Copy over all vars */
    newitem->type = item->type & (~cJSON_IsReference);
    newitem->valueint = item->valueint;
    newitem->valuedouble = item->valuedouble;
    if (item->valuestring)
    {
        newitem->valuestring = (char*)cJSON_strdup((unsigned char*)item->valuestring, &global_hooks);
        if (!newitem->valuestring)
        {
            goto fail;
        }
    }
    if (item->string)
    {
        newitem->string = (item->type&cJSON_StringIsConst) ? item->string : (char*)cJSON_strdup((unsigned char*)item->string, &global_hooks);
        if (!newitem->string)
        {
            goto fail;
        }
    }
    /* If non-recursive, then we're done! */
    if (!recurse)
    {
        return newitem;
    }
    /* Walk the ->next chain for the child. */
    child = item->child;
    while (child != NULL)
    {
        if(depth >= CJSON_CIRCULAR_LIMIT) {
            goto fail;
        }
        newchild = cJSON_Duplicate_rec(child, depth + 1, true); /* Duplicate (with recurse) each item in the ->next chain */
        if (!newchild)
        {
            goto fail;
        }
        if (next != NULL)
        {
            /* If newitem->child already set, then crosswire ->prev and ->next and move on */
            next->next = newchild;
            newchild->prev = next;
            next = newchild;
        }
        else
        {
            /* Set newitem->child and move to it */
            newitem->child = newchild;
            next = newchild;
        }
        child = child->next;
    }
    if (newitem && newitem->child)
    {
        newitem->child->prev = newchild;
    }

    return newitem;

fail:
    if (newitem != NULL)
    {
        cJSON_Delete(newitem);
    }

    return NULL;
}

static void skip_oneline_comment(char **input)
{
    *input += static_strlen("//");

    for (; (*input)[0] != '\0'; ++(*input))
    {
        if ((*input)[0] == '\n') {
            *input += static_strlen("\n");
            return;
        }
    }
}

static void skip_multiline_comment(char **input)
{
    *input += static_strlen("/*");

    for (; (*input)[0] != '\0'; ++(*input))
    {
        if (((*input)[0] == '*') && ((*input)[1] == '/'))
        {
            *input += static_strlen("*/");
            return;
        }
    }
}

static void minify_string(char **input, char **output) {
    (*output)[0] = (*input)[0];
    *input += static_strlen("\"");
    *output += static_strlen("\"");


    for (; (*input)[0] != '\0'; (void)++(*input), ++(*output)) {
        (*output)[0] = (*input)[0];

        if ((*input)[0] == '\"') {
            (*output)[0] = '\"';
            *input += static_strlen("\"");
            *output += static_strlen("\"");
            return;
        } else if (((*input)[0] == '\\') && ((*input)[1] == '\"')) {
            (*output)[1] = (*input)[1];
            *input += static_strlen("\"");
            *output += static_strlen("\"");
        }
    }
}

CJSON_PUBLIC(void) cJSON_Minify(char *json)
{
    char *into = json;

    if (json == NULL)
    {
        return;
    }

    while (json[0] != '\0')
    {
        switch (json[0])
        {
            case ' ':
            case '\t':
            case '\r':
            case '\n':
                json++;
                break;

            case '/':
                if (json[1] == '/')
                {
                    skip_oneline_comment(&json);
                }
                else if (json[1] == '*')
                {
                    skip_multiline_comment(&json);
                } else {
                    json++;
                }
                break;

            case '\"':
                minify_string(&json, (char**)&into);
                break;

            default:
                into[0] = json[0];
                json++;
                into++;
        }
    }

    /* and null-terminate. */
    *into = '\0';
}

CJSON_PUBLIC(cJSON_bool) cJSON_IsInvalid(const cJSON * const item)
{
    if (item == NULL)
    {
        return false;
    }

    return (item->type & 0xFF) == cJSON_Invalid;
}

CJSON_PUBLIC(cJSON_bool) cJSON_IsFalse(const cJSON * const item)
{
    if (item == NULL)
    {
        return false;
    }

    return (item->type & 0xFF) == cJSON_False;
}

CJSON_PUBLIC(cJSON_bool) cJSON_IsTrue(const cJSON * const item)
{
    if (item == NULL)
    {
        return false;
    }

    return (item->type & 0xff) == cJSON_True;
}


CJSON_PUBLIC(cJSON_bool) cJSON_IsBool(const cJSON * const item)
{
    if (item == NULL)
    {
        return false;
    }

    return (item->type & (cJSON_True | cJSON_False)) != 0;
}
CJSON_PUBLIC(cJSON_bool) cJSON_IsNull(const cJSON * const item)
{
    if (item == NULL)
    {
        return false;
    }

    return (item->type & 0xFF) == cJSON_NULL;
}

CJSON_PUBLIC(cJSON_bool) cJSON_IsNumber(const cJSON * const item)
{
    if (item == NULL)
    {
        return false;
    }

    return (item->type & 0xFF) == cJSON_Number;
}

CJSON_PUBLIC(cJSON_bool) cJSON_IsString(const cJSON * const item)
{
    if (item == NULL)
    {
        return false;
    }

    return (item->type & 0xFF) == cJSON_String;
}

CJSON_PUBLIC(cJSON_bool) cJSON_IsArray(const cJSON * const item)
{
    if (item == NULL)
    {
        return false;
    }

    return (item->type & 0xFF) == cJSON_Array;
}

CJSON_PUBLIC(cJSON_bool) cJSON_IsObject(const cJSON * const item)
{
    if (item == NULL)
    {
        return false;
    }

    return (item->type & 0xFF) == cJSON_Object;
}

CJSON_PUBLIC(cJSON_bool) cJSON_IsRaw(const cJSON * const item)
{
    if (item == NULL)
    {
        return false;
    }

    return (item->type & 0xFF) == cJSON_Raw;
}

CJSON_PUBLIC(cJSON_bool) cJSON_Compare(const cJSON * const a, const cJSON * const b, const cJSON_bool case_sensitive)
{
    if ((a == NULL) || (b == NULL) || ((a->type & 0xFF) != (b->type & 0xFF)))
    {
        return false;
    }

    /* check if type is valid */
    switch (a->type & 0xFF)
    {
        case cJSON_False:
        case cJSON_True:
        case cJSON_NULL:
        case cJSON_Number:
        case cJSON_String:
        case cJSON_Raw:
        case cJSON_Array:
        case cJSON_Object:
            break;

        default:
            return false;
    }

    /* identical objects are equal */
    if (a == b)
    {
        return true;
    }

    switch (a->type & 0xFF)
    {
        /* in these cases and equal type is enough */
        case cJSON_False:
        case cJSON_True:
        case cJSON_NULL:
            return true;

        case cJSON_Number:
            if (compare_double(a->valuedouble, b->valuedouble))
            {
                return true;
            }
            return false;

        case cJSON_String:
        case cJSON_Raw:
            if ((a->valuestring == NULL) || (b->valuestring == NULL))
            {
                return false;
            }
            if (strcmp(a->valuestring, b->valuestring) == 0)
            {
                return true;
            }

            return false;

        case cJSON_Array:
        {
            cJSON *a_element = a->child;
            cJSON *b_element = b->child;

            for (; (a_element != NULL) && (b_element != NULL);)
            {
                if (!cJSON_Compare(a_element, b_element, case_sensitive))
                {
                    return false;
                }

                a_element = a_element->next;
                b_element = b_element->next;
            }

            /* one of the arrays is longer than the other */
            if (a_element != b_element) {
                return false;
            }

            return true;
        }

        case cJSON_Object:
        {
            cJSON *a_element = NULL;
            cJSON *b_element = NULL;
            cJSON_ArrayForEach(a_element, a)
            {
                /* TODO This has O(n^2) runtime, which is horrible! */
                b_element = get_object_item(b, a_element->string, case_sensitive);
                if (b_element == NULL)
                {
                    return false;
                }

                if (!cJSON_Compare(a_element, b_element, case_sensitive))
                {
                    return false;
                }
            }

            /* doing this twice, once on a and b to prevent true comparison if a subset of b
             * TODO: Do this the proper way, this is just a fix for now */
            cJSON_ArrayForEach(b_element, b)
            {
                a_element = get_object_item(a, b_element->string, case_sensitive);
                if (a_element == NULL)
                {
                    return false;
                }

                if (!cJSON_Compare(b_element, a_element, case_sensitive))
                {
                    return false;
                }
            }

            return true;
        }

        default:
            return false;
    }
}

CJSON_PUBLIC(void *) cJSON_malloc(size_t size)
{
    return global_hooks.allocate(size);
}

CJSON_PUBLIC(void) cJSON_free(void *object)
{
    global_hooks.deallocate(object);
    object = NULL;
}
