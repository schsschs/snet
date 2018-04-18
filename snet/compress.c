/**
@file compress.c
@brief An adaptive order-2 PPM range coder
*/
#define SNET_BUILDING_LIB 1
#include <string.h>
#include "snet/snet.h"

typedef struct _SNetSymbol
{
	/* binary indexed tree of symbols */
	snet_uint8 value;
	snet_uint8 count;
	snet_uint16 under;
	snet_uint16 left, right;

	/* context defined by this symbol */
	snet_uint16 symbols;
	snet_uint16 escapes;
	snet_uint16 total;
	snet_uint16 parent;
} SNetSymbol;

/* adaptation constants tuned aggressively for small packet sizes rather than large file compression */
enum
{
	SNET_RANGE_CODER_TOP = 1 << 24,
	SNET_RANGE_CODER_BOTTOM = 1 << 16,

	SNET_CONTEXT_SYMBOL_DELTA = 3,
	SNET_CONTEXT_SYMBOL_MINIMUM = 1,
	SNET_CONTEXT_ESCAPE_MINIMUM = 1,

	SNET_SUBCONTEXT_ORDER = 2,
	SNET_SUBCONTEXT_SYMBOL_DELTA = 2,
	SNET_SUBCONTEXT_ESCAPE_DELTA = 5
};

/* context exclusion roughly halves compression speed, so disable for now */
#undef SNET_CONTEXT_EXCLUSION

typedef struct _SNetRangeCoder
{
	/* only allocate enough symbols for reasonable MTUs, would need to be larger for large file compression */
	SNetSymbol symbols[4096];
} SNetRangeCoder;

void *
snet_range_coder_create(void)
{
	SNetRangeCoder * rangeCoder = (SNetRangeCoder *)snet_malloc(sizeof(SNetRangeCoder));
	if (rangeCoder == NULL)
		return NULL;

	return rangeCoder;
}

void
snet_range_coder_destroy(void * context)
{
	SNetRangeCoder * rangeCoder = (SNetRangeCoder *)context;
	if (rangeCoder == NULL)
		return;

	snet_free(rangeCoder);
}

#define SNET_SYMBOL_CREATE(symbol, value_, count_) \
{ \
    symbol = & rangeCoder -> symbols [nextSymbol ++]; \
    symbol -> value = value_; \
    symbol -> count = count_; \
    symbol -> under = count_; \
    symbol -> left = 0; \
    symbol -> right = 0; \
    symbol -> symbols = 0; \
    symbol -> escapes = 0; \
    symbol -> total = 0; \
    symbol -> parent = 0; \
}

#define SNET_CONTEXT_CREATE(context, escapes_, minimum) \
{ \
    SNET_SYMBOL_CREATE (context, 0, 0); \
    (context) -> escapes = escapes_; \
    (context) -> total = escapes_ + 256*minimum; \
    (context) -> symbols = 0; \
}

static snet_uint16
snet_symbol_rescale(SNetSymbol * symbol)
{
	snet_uint16 total = 0;
	for (;;)
	{
		symbol->count -= symbol->count >> 1;
		symbol->under = symbol->count;
		if (symbol->left)
			symbol->under += snet_symbol_rescale(symbol + symbol->left);
		total += symbol->under;
		if (!symbol->right) break;
		symbol += symbol->right;
	}
	return total;
}

#define SNET_CONTEXT_RESCALE(context, minimum) \
{ \
    (context) -> total = (context) -> symbols ? snet_symbol_rescale ((context) + (context) -> symbols) : 0; \
    (context) -> escapes -= (context) -> escapes >> 1; \
    (context) -> total += (context) -> escapes + 256*minimum; \
}

#define SNET_RANGE_CODER_OUTPUT(value) \
{ \
    if (outData >= outEnd) \
      return 0; \
    * outData ++ = value; \
}

#define SNET_RANGE_CODER_ENCODE(under, count, total) \
{ \
    encodeRange /= (total); \
    encodeLow += (under) * encodeRange; \
    encodeRange *= (count); \
    for (;;) \
    { \
        if((encodeLow ^ (encodeLow + encodeRange)) >= SNET_RANGE_CODER_TOP) \
        { \
            if(encodeRange >= SNET_RANGE_CODER_BOTTOM) break; \
            encodeRange = -encodeLow & (SNET_RANGE_CODER_BOTTOM - 1); \
        } \
        SNET_RANGE_CODER_OUTPUT (encodeLow >> 24); \
        encodeRange <<= 8; \
        encodeLow <<= 8; \
    } \
}

#define SNET_RANGE_CODER_FLUSH \
{ \
    while (encodeLow) \
    { \
        SNET_RANGE_CODER_OUTPUT (encodeLow >> 24); \
        encodeLow <<= 8; \
    } \
}

#define SNET_RANGE_CODER_FREE_SYMBOLS \
{ \
    if (nextSymbol >= sizeof (rangeCoder -> symbols) / sizeof (SNetSymbol) - SNET_SUBCONTEXT_ORDER ) \
    { \
        nextSymbol = 0; \
        SNET_CONTEXT_CREATE (root, SNET_CONTEXT_ESCAPE_MINIMUM, SNET_CONTEXT_SYMBOL_MINIMUM); \
        predicted = 0; \
        order = 0; \
    } \
}

#define SNET_CONTEXT_ENCODE(context, symbol_, value_, under_, count_, update, minimum) \
{ \
    under_ = value*minimum; \
    count_ = minimum; \
    if (! (context) -> symbols) \
    { \
        SNET_SYMBOL_CREATE (symbol_, value_, update); \
        (context) -> symbols = symbol_ - (context); \
    } \
    else \
    { \
        SNetSymbol * node = (context) + (context) -> symbols; \
        for (;;) \
        { \
            if (value_ < node -> value) \
            { \
                node -> under += update; \
                if (node -> left) { node += node -> left; continue; } \
                SNET_SYMBOL_CREATE (symbol_, value_, update); \
                node -> left = symbol_ - node; \
            } \
            else \
            if (value_ > node -> value) \
            { \
                under_ += node -> under; \
                if (node -> right) { node += node -> right; continue; } \
                SNET_SYMBOL_CREATE (symbol_, value_, update); \
                node -> right = symbol_ - node; \
            } \
            else \
            { \
                count_ += node -> count; \
                under_ += node -> under - node -> count; \
                node -> under += update; \
                node -> count += update; \
                symbol_ = node; \
            } \
            break; \
        } \
    } \
}

#ifdef SNET_CONTEXT_EXCLUSION
static const SNetSymbol emptyContext = { 0, 0, 0, 0, 0, 0, 0, 0, 0 };

#define SNET_CONTEXT_WALK(context, body) \
{ \
    const SNetSymbol * node = (context) + (context) -> symbols; \
    const SNetSymbol * stack [256]; \
    size_t stackSize = 0; \
    while (node -> left) \
    { \
        stack [stackSize ++] = node; \
        node += node -> left; \
    } \
    for (;;) \
    { \
        body; \
        if (node -> right) \
        { \
            node += node -> right; \
            while (node -> left) \
            { \
                stack [stackSize ++] = node; \
                node += node -> left; \
            } \
        } \
        else \
        if (stackSize <= 0) \
            break; \
        else \
            node = stack [-- stackSize]; \
    } \
}

#define SNET_CONTEXT_ENCODE_EXCLUDE(context, value_, under, total, minimum) \
SNET_CONTEXT_WALK(context, { \
    if (node -> value != value_) \
    { \
        snet_uint16 parentCount = rangeCoder -> symbols [node -> parent].count + minimum; \
        if (node -> value < value_) \
          under -= parentCount; \
        total -= parentCount; \
    } \
})
#endif

size_t
snet_range_coder_compress(void * context, const SNetBuffer * inBuffers, size_t inBufferCount, size_t inLimit, snet_uint8 * outData, size_t outLimit)
{
	SNetRangeCoder * rangeCoder = (SNetRangeCoder *)context;
	snet_uint8 * outStart = outData, *outEnd = &outData[outLimit];
	const snet_uint8 * inData, *inEnd;
	snet_uint32 encodeLow = 0, encodeRange = ~0;
	SNetSymbol * root;
	snet_uint16 predicted = 0;
	size_t order = 0, nextSymbol = 0;

	if (rangeCoder == NULL || inBufferCount <= 0 || inLimit <= 0)
		return 0;

	inData = (const snet_uint8 *)inBuffers->data;
	inEnd = &inData[inBuffers->dataLength];
	inBuffers++;
	inBufferCount--;

	SNET_CONTEXT_CREATE(root, SNET_CONTEXT_ESCAPE_MINIMUM, SNET_CONTEXT_SYMBOL_MINIMUM);

	for (;;)
	{
		SNetSymbol * subcontext, *symbol;
#ifdef SNET_CONTEXT_EXCLUSION
		const SNetSymbol * childContext = &emptyContext;
#endif
		snet_uint8 value;
		snet_uint16 count, under, *parent = &predicted, total;
		if (inData >= inEnd)
		{
			if (inBufferCount <= 0)
				break;
			inData = (const snet_uint8 *)inBuffers->data;
			inEnd = &inData[inBuffers->dataLength];
			inBuffers++;
			inBufferCount--;
		}
		value = *inData++;

		for (subcontext = &rangeCoder->symbols[predicted];
			subcontext != root;
#ifdef SNET_CONTEXT_EXCLUSION
			childContext = subcontext,
#endif
			subcontext = &rangeCoder->symbols[subcontext->parent])
		{
			SNET_CONTEXT_ENCODE(subcontext, symbol, value, under, count, SNET_SUBCONTEXT_SYMBOL_DELTA, 0);
			*parent = symbol - rangeCoder->symbols;
			parent = &symbol->parent;
			total = subcontext->total;
#ifdef SNET_CONTEXT_EXCLUSION
			if (childContext->total > SNET_SUBCONTEXT_SYMBOL_DELTA + SNET_SUBCONTEXT_ESCAPE_DELTA)
				SNET_CONTEXT_ENCODE_EXCLUDE(childContext, value, under, total, 0);
#endif
			if (count > 0)
			{
				SNET_RANGE_CODER_ENCODE(subcontext->escapes + under, count, total);
			}
			else
			{
				if (subcontext->escapes > 0 && subcontext->escapes < total)
					SNET_RANGE_CODER_ENCODE(0, subcontext->escapes, total);
				subcontext->escapes += SNET_SUBCONTEXT_ESCAPE_DELTA;
				subcontext->total += SNET_SUBCONTEXT_ESCAPE_DELTA;
			}
			subcontext->total += SNET_SUBCONTEXT_SYMBOL_DELTA;
			if (count > 0xFF - 2 * SNET_SUBCONTEXT_SYMBOL_DELTA || subcontext->total > SNET_RANGE_CODER_BOTTOM - 0x100)
				SNET_CONTEXT_RESCALE(subcontext, 0);
			if (count > 0) goto nextInput;
		}

		SNET_CONTEXT_ENCODE(root, symbol, value, under, count, SNET_CONTEXT_SYMBOL_DELTA, SNET_CONTEXT_SYMBOL_MINIMUM);
		*parent = symbol - rangeCoder->symbols;
		parent = &symbol->parent;
		total = root->total;
#ifdef SNET_CONTEXT_EXCLUSION
		if (childContext->total > SNET_SUBCONTEXT_SYMBOL_DELTA + SNET_SUBCONTEXT_ESCAPE_DELTA)
			SNET_CONTEXT_ENCODE_EXCLUDE(childContext, value, under, total, SNET_CONTEXT_SYMBOL_MINIMUM);
#endif
		SNET_RANGE_CODER_ENCODE(root->escapes + under, count, total);
		root->total += SNET_CONTEXT_SYMBOL_DELTA;
		if (count > 0xFF - 2 * SNET_CONTEXT_SYMBOL_DELTA + SNET_CONTEXT_SYMBOL_MINIMUM || root->total > SNET_RANGE_CODER_BOTTOM - 0x100)
			SNET_CONTEXT_RESCALE(root, SNET_CONTEXT_SYMBOL_MINIMUM);

	nextInput:
		if (order >= SNET_SUBCONTEXT_ORDER)
			predicted = rangeCoder->symbols[predicted].parent;
		else
			order++;
		SNET_RANGE_CODER_FREE_SYMBOLS;
	}

	SNET_RANGE_CODER_FLUSH;

	return (size_t)(outData - outStart);
}

#define SNET_RANGE_CODER_SEED \
{ \
    if (inData < inEnd) decodeCode |= * inData ++ << 24; \
    if (inData < inEnd) decodeCode |= * inData ++ << 16; \
    if (inData < inEnd) decodeCode |= * inData ++ << 8; \
    if (inData < inEnd) decodeCode |= * inData ++; \
}

#define SNET_RANGE_CODER_READ(total) ((decodeCode - decodeLow) / (decodeRange /= (total)))

#define SNET_RANGE_CODER_DECODE(under, count, total) \
{ \
    decodeLow += (under) * decodeRange; \
    decodeRange *= (count); \
    for (;;) \
    { \
        if((decodeLow ^ (decodeLow + decodeRange)) >= SNET_RANGE_CODER_TOP) \
        { \
            if(decodeRange >= SNET_RANGE_CODER_BOTTOM) break; \
            decodeRange = -decodeLow & (SNET_RANGE_CODER_BOTTOM - 1); \
        } \
        decodeCode <<= 8; \
        if (inData < inEnd) \
          decodeCode |= * inData ++; \
        decodeRange <<= 8; \
        decodeLow <<= 8; \
    } \
}

#define SNET_CONTEXT_DECODE(context, symbol_, code, value_, under_, count_, update, minimum, createRoot, visitNode, createRight, createLeft) \
{ \
    under_ = 0; \
    count_ = minimum; \
    if (! (context) -> symbols) \
    { \
        createRoot; \
    } \
    else \
    { \
        SNetSymbol * node = (context) + (context) -> symbols; \
        for (;;) \
        { \
            snet_uint16 after = under_ + node -> under + (node -> value + 1)*minimum, before = node -> count + minimum; \
            visitNode; \
            if (code >= after) \
            { \
                under_ += node -> under; \
                if (node -> right) { node += node -> right; continue; } \
                createRight; \
            } \
            else \
            if (code < after - before) \
            { \
                node -> under += update; \
                if (node -> left) { node += node -> left; continue; } \
                createLeft; \
            } \
            else \
            { \
                value_ = node -> value; \
                count_ += node -> count; \
                under_ = after - before; \
                node -> under += update; \
                node -> count += update; \
                symbol_ = node; \
            } \
            break; \
        } \
    } \
}

#define SNET_CONTEXT_TRY_DECODE(context, symbol_, code, value_, under_, count_, update, minimum, exclude) \
SNET_CONTEXT_DECODE (context, symbol_, code, value_, under_, count_, update, minimum, return 0, exclude (node -> value, after, before), return 0, return 0)

#define SNET_CONTEXT_ROOT_DECODE(context, symbol_, code, value_, under_, count_, update, minimum, exclude) \
SNET_CONTEXT_DECODE (context, symbol_, code, value_, under_, count_, update, minimum, \
    { \
        value_ = code / minimum; \
        under_ = code - code%minimum; \
        SNET_SYMBOL_CREATE (symbol_, value_, update); \
        (context) -> symbols = symbol_ - (context); \
    }, \
    exclude (node -> value, after, before), \
    { \
        value_ = node->value + 1 + (code - after)/minimum; \
        under_ = code - (code - after)%minimum; \
        SNET_SYMBOL_CREATE (symbol_, value_, update); \
        node -> right = symbol_ - node; \
    }, \
    { \
        value_ = node->value - 1 - (after - before - code - 1)/minimum; \
        under_ = code - (after - before - code - 1)%minimum; \
        SNET_SYMBOL_CREATE (symbol_, value_, update); \
        node -> left = symbol_ - node; \
    }) \

#ifdef SNET_CONTEXT_EXCLUSION
typedef struct _SNetExclude
{
	snet_uint8 value;
	snet_uint16 under;
} SNetExclude;

#define SNET_CONTEXT_DECODE_EXCLUDE(context, total, minimum) \
{ \
    snet_uint16 under = 0; \
    nextExclude = excludes; \
    SNET_CONTEXT_WALK (context, { \
        under += rangeCoder -> symbols [node -> parent].count + minimum; \
        nextExclude -> value = node -> value; \
        nextExclude -> under = under; \
        nextExclude ++; \
    }); \
    total -= under; \
}

#define SNET_CONTEXT_EXCLUDED(value_, after, before) \
{ \
    size_t low = 0, high = nextExclude - excludes; \
    for(;;) \
    { \
        size_t mid = (low + high) >> 1; \
        const SNetExclude * exclude = & excludes [mid]; \
        if (value_ < exclude -> value) \
        { \
            if (low + 1 < high) \
            { \
                high = mid; \
                continue; \
            } \
            if (exclude > excludes) \
              after -= exclude [-1].under; \
        } \
        else \
        { \
            if (value_ > exclude -> value) \
            { \
                if (low + 1 < high) \
                { \
                    low = mid; \
                    continue; \
                } \
            } \
            else \
              before = 0; \
            after -= exclude -> under; \
        } \
        break; \
    } \
}
#endif

#define SNET_CONTEXT_NOT_EXCLUDED(value_, after, before)

size_t
snet_range_coder_decompress(void * context, const snet_uint8 * inData, size_t inLimit, snet_uint8 * outData, size_t outLimit)
{
	SNetRangeCoder * rangeCoder = (SNetRangeCoder *)context;
	snet_uint8 * outStart = outData, *outEnd = &outData[outLimit];
	const snet_uint8 * inEnd = &inData[inLimit];
	snet_uint32 decodeLow = 0, decodeCode = 0, decodeRange = ~0;
	SNetSymbol * root;
	snet_uint16 predicted = 0;
	size_t order = 0, nextSymbol = 0;
#ifdef SNET_CONTEXT_EXCLUSION
	SNetExclude excludes[256];
	SNetExclude * nextExclude = excludes;
#endif

	if (rangeCoder == NULL || inLimit <= 0)
		return 0;

	SNET_CONTEXT_CREATE(root, SNET_CONTEXT_ESCAPE_MINIMUM, SNET_CONTEXT_SYMBOL_MINIMUM);

	SNET_RANGE_CODER_SEED;

	for (;;)
	{
		SNetSymbol * subcontext, *symbol, *patch;
#ifdef SNET_CONTEXT_EXCLUSION
		const SNetSymbol * childContext = &emptyContext;
#endif
		snet_uint8 value = 0;
		snet_uint16 code, under, count, bottom, *parent = &predicted, total;

		for (subcontext = &rangeCoder->symbols[predicted];
			subcontext != root;
#ifdef SNET_CONTEXT_EXCLUSION
			childContext = subcontext,
#endif
			subcontext = &rangeCoder->symbols[subcontext->parent])
		{
			if (subcontext->escapes <= 0)
				continue;
			total = subcontext->total;
#ifdef SNET_CONTEXT_EXCLUSION
			if (childContext->total > 0)
				SNET_CONTEXT_DECODE_EXCLUDE(childContext, total, 0);
#endif
			if (subcontext->escapes >= total)
				continue;
			code = SNET_RANGE_CODER_READ(total);
			if (code < subcontext->escapes)
			{
				SNET_RANGE_CODER_DECODE(0, subcontext->escapes, total);
				continue;
			}
			code -= subcontext->escapes;
#ifdef SNET_CONTEXT_EXCLUSION
			if (childContext->total > 0)
			{
				SNET_CONTEXT_TRY_DECODE(subcontext, symbol, code, value, under, count, SNET_SUBCONTEXT_SYMBOL_DELTA, 0, SNET_CONTEXT_EXCLUDED);
			}
			else
#endif
			{
				SNET_CONTEXT_TRY_DECODE(subcontext, symbol, code, value, under, count, SNET_SUBCONTEXT_SYMBOL_DELTA, 0, SNET_CONTEXT_NOT_EXCLUDED);
			}
			bottom = symbol - rangeCoder->symbols;
			SNET_RANGE_CODER_DECODE(subcontext->escapes + under, count, total);
			subcontext->total += SNET_SUBCONTEXT_SYMBOL_DELTA;
			if (count > 0xFF - 2 * SNET_SUBCONTEXT_SYMBOL_DELTA || subcontext->total > SNET_RANGE_CODER_BOTTOM - 0x100)
				SNET_CONTEXT_RESCALE(subcontext, 0);
			goto patchContexts;
		}

		total = root->total;
#ifdef SNET_CONTEXT_EXCLUSION
		if (childContext->total > 0)
			SNET_CONTEXT_DECODE_EXCLUDE(childContext, total, SNET_CONTEXT_SYMBOL_MINIMUM);
#endif
		code = SNET_RANGE_CODER_READ(total);
		if (code < root->escapes)
		{
			SNET_RANGE_CODER_DECODE(0, root->escapes, total);
			break;
		}
		code -= root->escapes;
#ifdef SNET_CONTEXT_EXCLUSION
		if (childContext->total > 0)
		{
			SNET_CONTEXT_ROOT_DECODE(root, symbol, code, value, under, count, SNET_CONTEXT_SYMBOL_DELTA, SNET_CONTEXT_SYMBOL_MINIMUM, SNET_CONTEXT_EXCLUDED);
		}
		else
#endif
		{
			SNET_CONTEXT_ROOT_DECODE(root, symbol, code, value, under, count, SNET_CONTEXT_SYMBOL_DELTA, SNET_CONTEXT_SYMBOL_MINIMUM, SNET_CONTEXT_NOT_EXCLUDED);
		}
		bottom = symbol - rangeCoder->symbols;
		SNET_RANGE_CODER_DECODE(root->escapes + under, count, total);
		root->total += SNET_CONTEXT_SYMBOL_DELTA;
		if (count > 0xFF - 2 * SNET_CONTEXT_SYMBOL_DELTA + SNET_CONTEXT_SYMBOL_MINIMUM || root->total > SNET_RANGE_CODER_BOTTOM - 0x100)
			SNET_CONTEXT_RESCALE(root, SNET_CONTEXT_SYMBOL_MINIMUM);

	patchContexts:
		for (patch = &rangeCoder->symbols[predicted];
			patch != subcontext;
			patch = &rangeCoder->symbols[patch->parent])
		{
			SNET_CONTEXT_ENCODE(patch, symbol, value, under, count, SNET_SUBCONTEXT_SYMBOL_DELTA, 0);
			*parent = symbol - rangeCoder->symbols;
			parent = &symbol->parent;
			if (count <= 0)
			{
				patch->escapes += SNET_SUBCONTEXT_ESCAPE_DELTA;
				patch->total += SNET_SUBCONTEXT_ESCAPE_DELTA;
			}
			patch->total += SNET_SUBCONTEXT_SYMBOL_DELTA;
			if (count > 0xFF - 2 * SNET_SUBCONTEXT_SYMBOL_DELTA || patch->total > SNET_RANGE_CODER_BOTTOM - 0x100)
				SNET_CONTEXT_RESCALE(patch, 0);
		}
		*parent = bottom;

		SNET_RANGE_CODER_OUTPUT(value);

		if (order >= SNET_SUBCONTEXT_ORDER)
			predicted = rangeCoder->symbols[predicted].parent;
		else
			order++;
		SNET_RANGE_CODER_FREE_SYMBOLS;
	}

	return (size_t)(outData - outStart);
}

/** @defgroup host SNet host functions
@{
*/

/** Sets the packet compressor the host should use to the default range coder.
@param host host to enable the range coder for
@returns 0 on success, < 0 on failure
*/
int
snet_host_compress_with_range_coder(SNetHost * host)
{
	SNetCompressor compressor;
	memset(&compressor, 0, sizeof(compressor));
	compressor.context = snet_range_coder_create();
	if (compressor.context == NULL)
		return -1;
	compressor.compress = snet_range_coder_compress;
	compressor.decompress = snet_range_coder_decompress;
	compressor.destroy = snet_range_coder_destroy;
	snet_host_compress(host, &compressor);
	return 0;
}

/** @} */


