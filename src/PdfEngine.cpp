/* Copyright 2018 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#pragma warning(disable : 4611) // interaction between '_setjmp' and C++ object destruction is non-portable

extern "C" {
#include <mupdf/fitz.h>
}

#include "utils/BaseUtil.h"
#include "utils/Archive.h"
#include "utils/ScopedWin.h"
#include "utils/FileUtil.h"
#include "utils/HtmlParserLookup.h"
#include "utils/HtmlPullParser.h"
#include "utils/TrivialHtmlParser.h"
#include "utils/WinUtil.h"
#include "utils/ZipUtil.h"

#include "Colors.h"
#include "TreeModel.h"
#include "BaseEngine.h"
#include "PdfEngine.h"

// maximum size of a file that's entirely loaded into memory before parsed
// and displayed; larger files will be kept open while they're displayed
// so that their content can be loaded on demand in order to preserve memory
#define MAX_MEMORY_FILE_SIZE (10 * 1024 * 1024)

// number of page content trees to cache for quicker rendering
#define MAX_PAGE_RUN_CACHE 8
// maximum estimated memory requirement allowed for the run cache of one document
#define MAX_PAGE_RUN_MEMORY (40 * 1024 * 1024)

// maximum amount of memory that MuPDF should use per fz_context store
#define MAX_CONTEXT_MEMORY (256 * 1024 * 1024)

///// extensions to Fitz that are usable for both PDF and XPS /////

inline RectD fz_rect_to_RectD(fz_rect rect) {
    return RectD::FromXY(rect.x0, rect.y0, rect.x1, rect.y1);
}

inline fz_rect fz_RectD_to_rect(RectD rect) {
    fz_rect result = {(float)rect.x, (float)rect.y, (float)(rect.x + rect.dx), (float)(rect.y + rect.dy)};
    return result;
}

inline bool fz_is_pt_in_rect(fz_rect rect, fz_point pt) {
    return fz_rect_to_RectD(rect).Contains(PointD(pt.x, pt.y));
}

inline float fz_calc_overlap(fz_rect r1, fz_rect r2) {
    if (fz_is_empty_rect(r1))
        return 0.0f;
    fz_rect isect = fz_intersect_rect(r1, r2);
    return (isect.x1 - isect.x0) * (isect.y1 - isect.y0) / ((r1.x1 - r1.x0) * (r1.y1 - r1.y0));
}

// try to produce an 8-bit palette for saving some memory
static RenderedBitmap* try_render_as_palette_image(fz_pixmap* pixmap) {
    int w = pixmap->w;
    int h = pixmap->h;
    int rows8 = ((w + 3) / 4) * 4;
    unsigned char* bmpData = (unsigned char*)calloc(rows8, h);
    if (!bmpData)
        return nullptr;

    ScopedMem<BITMAPINFO> bmi((BITMAPINFO*)calloc(1, sizeof(BITMAPINFO) + 255 * sizeof(RGBQUAD)));

    unsigned char* dest = bmpData;
    unsigned char* source = pixmap->samples;
    uint32_t* palette = (uint32_t*)bmi.Get()->bmiColors;
    BYTE grayIdxs[256] = {0};

    int paletteSize = 0;
    RGBQUAD c;
    for (int j = 0; j < h; j++) {
        for (int i = 0; i < w; i++) {
            c.rgbRed = *source++;
            c.rgbGreen = *source++;
            c.rgbBlue = *source++;
            c.rgbReserved = 0;
            source++;

            /* find this color in the palette */
            int k;
            bool isGray = c.rgbRed == c.rgbGreen && c.rgbRed == c.rgbBlue;
            if (isGray) {
                k = grayIdxs[c.rgbRed] || palette[0] == *(uint32_t*)&c ? grayIdxs[c.rgbRed] : paletteSize;
            } else {
                for (k = 0; k < paletteSize && palette[k] != *(uint32_t*)&c; k++)
                    ;
            }
            /* add it to the palette if it isn't in there and if there's still space left */
            if (k == paletteSize) {
                if (++paletteSize > 256) {
                    free(bmpData);
                    return nullptr;
                }
                if (isGray) {
                    grayIdxs[c.rgbRed] = (BYTE)k;
                }
                palette[k] = *(uint32_t*)&c;
            }
            /* 8-bit data consists of indices into the color palette */
            *dest++ = k;
        }
        dest += rows8 - w;
    }

    BITMAPINFOHEADER* bmih = &bmi.Get()->bmiHeader;
    bmih->biSize = sizeof(*bmih);
    bmih->biWidth = w;
    bmih->biHeight = -h;
    bmih->biPlanes = 1;
    bmih->biCompression = BI_RGB;
    bmih->biBitCount = 8;
    bmih->biSizeImage = h * rows8;
    bmih->biClrUsed = paletteSize;

    void* data = nullptr;
    HANDLE hMap = CreateFileMapping(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, bmih->biSizeImage, nullptr);
    HBITMAP hbmp = CreateDIBSection(nullptr, bmi, DIB_RGB_COLORS, &data, hMap, 0);
    if (!hbmp) {
        free(bmpData);
        return nullptr;
    }
    memcpy(data, bmpData, bmih->biSizeImage);
    free(bmpData);
    return new RenderedBitmap(hbmp, SizeI(w, h), hMap);
}

static RenderedBitmap* new_rendered_fz_pixmap(fz_context* ctx, fz_pixmap* pixmap) {
    if (pixmap->n == 4 && fz_colorspace_is_rgb(ctx, pixmap->colorspace)) {
        RenderedBitmap* res = try_render_as_palette_image(pixmap);
        if (res) {
            return res;
        }
    }

    int w = pixmap->w;
    int h = pixmap->h;
    int rows8 = ((w + 3) / 4) * 4;

    ScopedMem<BITMAPINFO> bmi((BITMAPINFO*)calloc(1, sizeof(BITMAPINFO) + 255 * sizeof(RGBQUAD)));

    fz_pixmap* bgrPixmap = nullptr;
    /* BGRA is a GDI compatible format */
    fz_try(ctx) {
        fz_irect bbox = fz_pixmap_bbox(ctx, pixmap);
        fz_colorspace* csdest = fz_device_bgr(ctx);
        fz_color_params cp = fz_default_color_params;
        bgrPixmap = fz_convert_pixmap(ctx, pixmap, csdest, nullptr, nullptr, cp, 1);
    }
    fz_catch(ctx) {
        return nullptr;
    }

    BITMAPINFOHEADER* bmih = &bmi.Get()->bmiHeader;
    bmih->biSize = sizeof(*bmih);
    bmih->biWidth = w;
    bmih->biHeight = -h;
    bmih->biPlanes = 1;
    bmih->biCompression = BI_RGB;
    bmih->biBitCount = 32;
    bmih->biSizeImage = h * w * 4;
    bmih->biClrUsed = 0;

    void* data = nullptr;
    HANDLE hMap = CreateFileMapping(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, bmih->biSizeImage, nullptr);
    HBITMAP hbmp = CreateDIBSection(nullptr, bmi, DIB_RGB_COLORS, &data, hMap, 0);
    if (data) {
        memcpy(data, bgrPixmap->samples, bmih->biSizeImage);
    }
    fz_drop_pixmap(ctx, bgrPixmap);
    if (!hbmp) {
        return nullptr;
    }
    // return a RenderedBitmap even if hbmp is nullptr so that callers can
    // distinguish rendering errors from GDI resource exhaustion
    // (and in the latter case retry using smaller target rectangles)
    return new RenderedBitmap(hbmp, SizeI(w, h), hMap);
}

fz_stream* fz_open_file2(fz_context* ctx, const WCHAR* filePath) {
    fz_stream* stm = nullptr;
    int64_t fileSize = file::GetSize(filePath);
    // load small files entirely into memory so that they can be
    // overwritten even by programs that don't open files with FILE_SHARE_READ
    if (fileSize > 0 && fileSize < MAX_MEMORY_FILE_SIZE) {
        size_t size;
        char* data = file::ReadFileWithAllocator(filePath, &size, nullptr);
        if (data == nullptr) {
            // failed to read
            return nullptr;
        }
        fz_buffer* buf = fz_new_buffer_from_data(ctx, (u8*)data, size);
        fz_var(buf);
        fz_try(ctx) {
            stm = fz_open_buffer(ctx, buf);
        }
        fz_always(ctx) {
            fz_drop_buffer(ctx, buf);
        }
        fz_catch(ctx) {
            stm = nullptr;
        }
        return stm;
    }

    fz_try(ctx) {
        stm = fz_open_file_w(ctx, filePath);
    }
    fz_catch(ctx) {
        stm = nullptr;
    }
    return stm;
}

unsigned char* fz_extract_stream_data(fz_context* ctx, fz_stream* stream, size_t* cbCount) {
    fz_seek(ctx, stream, 0, 2);
    i64 fileLen = fz_tell(ctx, stream);
    fz_seek(ctx, stream, 0, 0);

    fz_buffer* buf = fz_read_all(ctx, stream, fileLen);

    u8* data;
    size_t size = fz_buffer_extract(ctx, buf, &data);
    CrashIf((size_t)fileLen != size);
    if (cbCount)
        *cbCount = size;

    fz_drop_buffer(ctx, buf);

    if (!data)
        fz_throw(ctx, FZ_ERROR_GENERIC, "OOM in fz_extract_stream_data");
    return data;
}

void fz_stream_fingerprint(fz_context* ctx, fz_stream* stm, unsigned char digest[16]) {
    i64 fileLen = -1;
    fz_buffer* buf = nullptr;

    fz_try(ctx) {
        fz_seek(ctx, stm, 0, 2);
        fileLen = fz_tell(ctx, stm);
        fz_seek(ctx, stm, 0, 0);
        buf = fz_read_all(ctx, stm, fileLen);
    }
    fz_catch(ctx) {
        fz_warn(ctx, "couldn't read stream data, using a nullptr fingerprint instead");
        ZeroMemory(digest, 16);
        return;
    }
    CrashIf(nullptr == buf);
    u8* data;
    size_t size = fz_buffer_extract(ctx, buf, &data);
    CrashIf((size_t)fileLen != size);
    fz_drop_buffer(ctx, buf);

    fz_md5 md5;
    fz_md5_init(&md5);
    fz_md5_update(&md5, data, size);
    fz_md5_final(&md5, digest);
}

static inline int wchars_per_rune(int rune) {
    if (rune & 0x1F0000)
        return 2;
    return 1;
}

static void AddChar(fz_stext_line* line, fz_stext_char* c, str::Str<WCHAR>& s, Vec<RectI>& rects) {
    fz_rect bbox = fz_rect_from_quad(c->quad);
    RectI r = fz_rect_to_RectD(bbox).Round();

    int n = wchars_per_rune(c->c);
    if (n == 2) {
        WCHAR tmp[2];
        tmp[0] = 0xD800 | ((c->c - 0x10000) >> 10) & 0x3FF;
        tmp[1] = 0xDC00 | (c->c - 0x10000) & 0x3FF;
        s.Append(tmp, 2);
        rects.Append(r);
        rects.Append(r);
        return;
    }
    WCHAR wc = c->c;
    bool isNonPrintable = (wc <= 32) || str::IsNonCharacter(wc);
    if (!isNonPrintable) {
        s.Append(wc);
        rects.Append(r);
        return;
    }

    // non-printable or whitespace
    if (!str::IsWs(wc)) {
        s.Append(L'?');
        rects.Append(r);
        return;
    }

    // collapse multiple whitespace characters into one
    WCHAR prev = s.LastChar();
    if (!str::IsWs(prev)) {
        s.Append(L' ');
        rects.Append(r);
    }
}

#if 0
// if there's a span following this one, add space to separate them
static void AddSpaceAtLineEnd(fz_stext_line* line, str::Str<WCHAR>& s, Vec<RectI>& rects) {
    if (line->first_char == line->last_char || line->next == NULL) {
        return;
    }
    CrashIf(s.size() == 0);
    CrashIf(rects.size() == 0);
    if (s.LastChar() == ' ') {
        return;
    }
    // TODO: use a Tab instead? (this might be a table)
    s.Append(L' ');
    RectI prev = rects.Last();
    prev.x += prev.dx;
    prev.dx /= 2;
    rects.Append(prev);
}
#endif

static void AddLineSep(str::Str<WCHAR>& s, Vec<RectI>& rects, const WCHAR* lineSep, size_t lineSepLen) {
    if (lineSepLen == 0) {
        return;
    }
    // remove trailing spaces
    if (str::IsWs(s.LastChar())) {
        s.Pop();
        rects.Pop();
    }

    s.Append(lineSep);
    for (size_t i = 0; i < lineSepLen; i++) {
        rects.Append(RectI());
    }
}

static WCHAR* fz_text_page_to_str(fz_stext_page* text, const WCHAR* lineSep, RectI** coordsOut) {
    size_t lineSepLen = str::Len(lineSep);
    str::Str<WCHAR> content;
    // coordsOut is optional but we ask for it by default so we simplify the code
    // by always calculating it
    Vec<RectI> rects;

    fz_stext_block* block = text->first_block;
    while (block) {
        if (block->type != FZ_STEXT_BLOCK_TEXT) {
            block = block->next;
            continue;
        }
        fz_stext_line* line = block->u.t.first_line;
        while (line) {
            fz_stext_char* c = line->first_char;
            while (c) {
                AddChar(line, c, content, rects);
                c = c->next;
            }
            AddLineSep(content, rects, lineSep, lineSepLen);
            line = line->next;
        }

        block = block->next;
    }

    CrashIf(content.size() != rects.size());

    if (coordsOut) {
        *coordsOut = rects.StealData();
    }

    return content.StealData();
}

struct istream_filter {
    IStream* stream;
    unsigned char buf[4096];
};

extern "C" static int next_istream(fz_context* ctx, fz_stream* stm, size_t max) {
    UNUSED(max);
    istream_filter* state = (istream_filter*)stm->state;
    ULONG cbRead = sizeof(state->buf);
    HRESULT res = state->stream->Read(state->buf, sizeof(state->buf), &cbRead);
    if (FAILED(res))
        fz_throw(ctx, FZ_ERROR_GENERIC, "IStream read error: %x", res);
    stm->rp = state->buf;
    stm->wp = stm->rp + cbRead;
    stm->pos += cbRead;

    return cbRead > 0 ? *stm->rp++ : EOF;
}

extern "C" static void seek_istream(fz_context* ctx, fz_stream* stm, i64 offset, int whence) {
    istream_filter* state = (istream_filter*)stm->state;
    LARGE_INTEGER off;
    ULARGE_INTEGER n;
    off.QuadPart = offset;
    HRESULT res = state->stream->Seek(off, whence, &n);
    if (FAILED(res))
        fz_throw(ctx, FZ_ERROR_GENERIC, "IStream seek error: %x", res);
    if (n.HighPart != 0 || n.LowPart > INT_MAX)
        fz_throw(ctx, FZ_ERROR_GENERIC, "documents beyond 2GB aren't supported");
    stm->pos = n.LowPart;
    stm->rp = stm->wp = state->buf;
}

extern "C" static void drop_istream(fz_context* ctx, void* state_) {
    istream_filter* state = (istream_filter*)state_;
    state->stream->Release();
    fz_free(ctx, state);
}

fz_stream* fz_open_istream(fz_context* ctx, IStream* stream);

// TODO:(port)
#if 0
extern "C" static fz_stream* reopen_istream(fz_context* ctx, fz_stream* stm) {
    istream_filter* state = (istream_filter*)stm->state;
    ScopedComPtr<IStream> stream2;
    HRESULT res = state->stream->Clone(&stream2);
    if (E_NOTIMPL == res)
        fz_throw(ctx, FZ_ERROR_GENERIC, "IStream doesn't support cloning");
    if (FAILED(res))
        fz_throw(ctx, FZ_ERROR_GENERIC, "IStream clone error: %x", res);
    return fz_open_istream(ctx, stream2);
}
#endif

fz_stream* fz_open_istream(fz_context* ctx, IStream* stream) {
    if (!stream)
        return nullptr;

    LARGE_INTEGER zero = {0};
    HRESULT res = stream->Seek(zero, STREAM_SEEK_SET, nullptr);
    if (FAILED(res))
        fz_throw(ctx, FZ_ERROR_GENERIC, "IStream seek error: %x", res);

    istream_filter* state = fz_malloc_struct(ctx, istream_filter);
    state->stream = stream;
    stream->AddRef();

    fz_stream* stm = fz_new_stream(ctx, state, next_istream, drop_istream);
    stm->seek = seek_istream;
    return stm;
}

fz_matrix fz_create_view_ctm(fz_rect mediabox, float zoom, int rotation) {
    fz_matrix ctm = fz_pre_scale(fz_rotate((float)rotation), zoom, zoom);

    AssertCrash(0 == mediabox.x0 && 0 == mediabox.y0);
    rotation = (rotation + 360) % 360;
    if (90 == rotation)
        ctm = fz_pre_translate(ctm, 0, -mediabox.y1);
    else if (180 == rotation)
        ctm = fz_pre_translate(ctm, -mediabox.x1, -mediabox.y1);
    else if (270 == rotation)
        ctm = fz_pre_translate(ctm, -mediabox.x1, 0);

    AssertCrash(fz_matrix_expansion(ctm) > 0);
    if (fz_matrix_expansion(ctm) == 0)
        return fz_identity;

    return ctm;
}

struct LinkRectList {
    WStrVec links;
    Vec<fz_rect> coords;
};

// TODO(port)
#if 0
static bool LinkifyCheckMultiline(const WCHAR* pageText, const WCHAR* pos, RectI* coords) {
    // multiline links end in a non-alphanumeric character and continue on a line
    // that starts left and only slightly below where the current line ended
    // (and that doesn't start with http or a footnote numeral)
    return '\n' == *pos && pos > pageText && *(pos + 1) && !iswalnum(pos[-1]) && !str::IsWs(pos[1]) &&
           coords[pos - pageText + 1].BR().y > coords[pos - pageText - 1].y &&
           coords[pos - pageText + 1].y <= coords[pos - pageText - 1].BR().y + coords[pos - pageText - 1].dy * 0.35 &&
           coords[pos - pageText + 1].x < coords[pos - pageText - 1].BR().x &&
           coords[pos - pageText + 1].dy >= coords[pos - pageText - 1].dy * 0.85 &&
           coords[pos - pageText + 1].dy <= coords[pos - pageText - 1].dy * 1.2 && !str::StartsWith(pos + 1, L"http");
}
#endif

// TODO(port)
#if 0
static const WCHAR* LinkifyFindEnd(const WCHAR* start, WCHAR prevChar) {
    const WCHAR *end, *quote;

    // look for the end of the URL (ends in a space preceded maybe by interpunctuation)
    for (end = start; *end && !str::IsWs(*end); end++)
        ;
    if (',' == end[-1] || '.' == end[-1] || '?' == end[-1] || '!' == end[-1])
        end--;
    // also ignore a closing parenthesis, if the URL doesn't contain any opening one
    if (')' == end[-1] && (!str::FindChar(start, '(') || str::FindChar(start, '(') >= end))
        end--;
    // cut the link at the first quotation mark, if it's also preceded by one
    if (('"' == prevChar || '\'' == prevChar) && (quote = str::FindChar(start, prevChar)) != nullptr && quote < end)
        end = quote;

    return end;
}
#endif

// TODO(port)
#if 0
static const WCHAR* LinkifyMultilineText(LinkRectList* list, const WCHAR* pageText, const WCHAR* start,
                                         const WCHAR* next, RectI* coords) {
    size_t lastIx = list->coords.size() - 1;
    AutoFreeW uri(list->links.at(lastIx));
    const WCHAR* end = next;
    bool multiline = false;

    do {
        end = LinkifyFindEnd(next, start > pageText ? start[-1] : ' ');
        multiline = LinkifyCheckMultiline(pageText, end, coords);

        AutoFreeW part(str::DupN(next, end - next));
        uri.Set(str::Join(uri, part));
        RectI bbox = coords[next - pageText].Union(coords[end - pageText - 1]);
        list->coords.Append(fz_RectD_to_rect(bbox.Convert<double>()));

        next = end + 1;
    } while (multiline);

    // update the link URL for all partial links
    list->links.at(lastIx) = str::Dup(uri);
    for (size_t i = lastIx + 1; i < list->coords.size(); i++)
        list->links.Append(str::Dup(uri));

    return end;
}
#endif

// cf. http://weblogs.mozillazine.org/gerv/archives/2011/05/html5_email_address_regexp.html
inline bool IsEmailUsernameChar(WCHAR c) {
    // explicitly excluding the '/' from the list, as it is more
    // often part of a URL or path than of an email address
    return iswalnum(c) || c && str::FindChar(L".!#$%&'*+=?^_`{|}~-", c);
}
inline bool IsEmailDomainChar(WCHAR c) {
    return iswalnum(c) || '-' == c;
}

#if 0
static const WCHAR* LinkifyFindEmail(const WCHAR* pageText, const WCHAR* at) {
    const WCHAR* start;
    for (start = at; start > pageText&& IsEmailUsernameChar(*(start - 1)); start--) {
        // do nothing
    }
    return start != at ? start : nullptr;
}
#endif

#if 0
static const WCHAR* LinkifyEmailAddress(const WCHAR* start) {
    const WCHAR* end;
    for (end = start; IsEmailUsernameChar(*end); end++)
        ;
    if (end == start || *end != '@' || !IsEmailDomainChar(*(end + 1)))
        return nullptr;
    for (end++; IsEmailDomainChar(*end); end++)
        ;
    if ('.' != *end || !IsEmailDomainChar(*(end + 1)))
        return nullptr;
    do {
        for (end++; IsEmailDomainChar(*end); end++)
            ;
    } while ('.' == *end && IsEmailDomainChar(*(end + 1)));
    return end;
}
#endif

// caller needs to delete the result
// TODO(port)
#if 0
static LinkRectList* LinkifyText(const WCHAR* pageText, RectI* coords) {
    LinkRectList* list = new LinkRectList;

    for (const WCHAR* start = pageText; *start; start++) {
        const WCHAR* end = nullptr;
        bool multiline = false;
        const WCHAR* protocol = nullptr;

        if ('@' == *start) {
            // potential email address without mailto:
            const WCHAR* email = LinkifyFindEmail(pageText, start);
            end = email ? LinkifyEmailAddress(email) : nullptr;
            protocol = L"mailto:";
            if (end != nullptr)
                start = email;
        } else if (start > pageText && ('/' == start[-1] || iswalnum(start[-1]))) {
            // hyperlinks must not be preceded by a slash (indicates a different protocol)
            // or an alphanumeric character (indicates part of a different protocol)
        } else if ('h' == *start && str::Parse(start, L"http%?s://")) {
            end = LinkifyFindEnd(start, start > pageText ? start[-1] : ' ');
            multiline = LinkifyCheckMultiline(pageText, end, coords);
        } else if ('w' == *start && str::StartsWith(start, L"www.")) {
            end = LinkifyFindEnd(start, start > pageText ? start[-1] : ' ');
            multiline = LinkifyCheckMultiline(pageText, end, coords);
            protocol = L"http://";
            // ignore www. links without a top-level domain
            if (end - start <= 4 || !multiline && (!wcschr(start + 5, '.') || wcschr(start + 5, '.') >= end))
                end = nullptr;
        } else if ('m' == *start && str::StartsWith(start, L"mailto:")) {
            end = LinkifyEmailAddress(start + 7);
        }
        if (!end)
            continue;

        AutoFreeW part(str::DupN(start, end - start));
        WCHAR* uri = protocol ? str::Join(protocol, part) : part.StealData();
        list->links.Append(uri);
        RectI bbox = coords[start - pageText].Union(coords[end - pageText - 1]);
        list->coords.Append(fz_RectD_to_rect(bbox.Convert<double>()));
        if (multiline)
            end = LinkifyMultilineText(list, pageText, start, end + 1, coords);

        start = end;
    }

    return list;
}
#endif

static fz_link* FixupPageLinks(fz_link* root) {
    // Links in PDF documents are added from bottom-most to top-most,
    // i.e. links that appear later in the list should be preferred
    // to links appearing before. Since we search from the start of
    // the (single-linked) list, we have to reverse the order of links
    // (cf. http://code.google.com/p/sumatrapdf/issues/detail?id=1303 )
    fz_link* new_root = nullptr;
    while (root) {
        fz_link* tmp = root->next;
        root->next = new_root;
        new_root = root;
        root = tmp;

        // there are PDFs that have x,y positions in reverse order, so fix them up
        fz_link* link = new_root;
        if (link->rect.x0 > link->rect.x1)
            std::swap(link->rect.x0, link->rect.x1);
        if (link->rect.y0 > link->rect.y1)
            std::swap(link->rect.y0, link->rect.y1);
        AssertCrash(link->rect.x1 >= link->rect.x0);
        AssertCrash(link->rect.y1 >= link->rect.y0);
    }
    return new_root;
}

class SimpleDest : public PageDestination {
    int pageNo;
    RectD rect;

  public:
    SimpleDest(int pageNo, RectD rect) : pageNo(pageNo), rect(rect) {
    }

    PageDestType GetDestType() const override {
        return PageDestType::ScrollTo;
    }
    int GetDestPageNo() const override {
        return pageNo;
    }
    RectD GetDestRect() const override {
        return rect;
    }
};

struct FitzImagePos {
    fz_image* image;
    fz_rect rect;

    explicit FitzImagePos(fz_image* image = nullptr, fz_rect rect = fz_unit_rect) : image(image), rect(rect) {
    }
};

class FitzAbortCookie : public AbortCookie {
  public:
    fz_cookie cookie;
    FitzAbortCookie() {
        memset(&cookie, 0, sizeof(cookie));
    }
    void Abort() override {
        cookie.abort = 1;
    }
};

static Vec<PageAnnotation> fz_get_user_page_annots(Vec<PageAnnotation>& userAnnots, int pageNo) {
    Vec<PageAnnotation> result;
    for (size_t i = 0; i < userAnnots.size(); i++) {
        PageAnnotation& annot = userAnnots.at(i);
        if (annot.pageNo != pageNo)
            continue;
        // include all annotations for pageNo that can be rendered by fz_run_user_annots
        switch (annot.type) {
            case PageAnnotType::Highlight:
            case PageAnnotType::Underline:
            case PageAnnotType::StrikeOut:
            case PageAnnotType::Squiggly:
                result.Append(annot);
                break;
        }
    }
    return result;
}

// TODO(port)
#if 0
static void fz_run_user_page_annots(fz_context* ctx, Vec<PageAnnotation>& pageAnnots, fz_device* dev, fz_matrix ctm,
                                    const fz_rect* cliprect, fz_cookie* cookie) {
    for (size_t i = 0; i < pageAnnots.size() && (!cookie || !cookie->abort); i++) {
        PageAnnotation& annot = pageAnnots.at(i);
        // skip annotation if it isn't visible
        fz_rect rect = fz_RectD_to_rect(annot.rect);
        rect = fz_transform_rect(rect, ctm);
        if (cliprect && fz_is_empty_rect(fz_intersect_rect(rect, *cliprect))) {
            continue;
        }
        // prepare text highlighting path (cf. pdf_create_highlight_annot
        // and pdf_create_markup_annot in pdf_annot.c)
        fz_path* path = fz_new_path(ctx);
        fz_stroke_state* stroke = nullptr;
        switch (annot.type) {
            case PageAnnotType::Highlight:
                fz_moveto(ctx, path, annot.rect.TL().x, annot.rect.TL().y);
                fz_lineto(ctx, path, annot.rect.BR().x, annot.rect.TL().y);
                fz_lineto(ctx, path, annot.rect.BR().x, annot.rect.BR().y);
                fz_lineto(ctx, path, annot.rect.TL().x, annot.rect.BR().y);
                fz_closepath(ctx, path);
                break;
            case PageAnnotType::Underline:
                fz_moveto(ctx, path, annot.rect.TL().x, annot.rect.BR().y - 0.25f);
                fz_lineto(ctx, path, annot.rect.BR().x, annot.rect.BR().y - 0.25f);
                break;
            case PageAnnotType::StrikeOut:
                fz_moveto(ctx, path, annot.rect.TL().x, annot.rect.TL().y + annot.rect.dy / 2);
                fz_lineto(ctx, path, annot.rect.BR().x, annot.rect.TL().y + annot.rect.dy / 2);
                break;
            case PageAnnotType::Squiggly:
                fz_moveto(ctx, path, annot.rect.TL().x + 1, annot.rect.BR().y);
                fz_lineto(ctx, path, annot.rect.BR().x, annot.rect.BR().y);
                fz_moveto(ctx, path, annot.rect.TL().x, annot.rect.BR().y - 0.5f);
                fz_lineto(ctx, path, annot.rect.BR().x, annot.rect.BR().y - 0.5f);
                stroke = fz_new_stroke_state_with_dash_len(ctx, 2);
                CrashIf(!stroke);
                stroke->linewidth = 0.5f;
                stroke->dash_list[stroke->dash_len++] = 1;
                stroke->dash_list[stroke->dash_len++] = 1;
                break;
            default:
                CrashIf(true);
        }
        fz_colorspace* cs = fz_device_rgb(ctx);
        float color[3] = {annot.color.r / 255.f, annot.color.g / 255.f, annot.color.b / 255.f};
        if (PageAnnotType::Highlight == annot.type) {
            // render path with transparency effect
            fz_begin_group(ctx, dev, rect, nullptr, 0, 0, FZ_BLEND_MULTIPLY, 1.f);
            fz_fill_path(ctx, dev, path, 0, ctm, cs, color, annot.color.a / 255.f, fz_default_color_params);
            fz_end_group(ctx, dev);
        } else {
            if (!stroke)
                stroke = fz_new_stroke_state(ctx);
            fz_stroke_path(ctx, dev, path, stroke, ctm, cs, color, 1.0f, fz_default_color_params);
            fz_drop_stroke_state(ctx, stroke);
        }
        fz_drop_path(ctx, path);
    }
}
#endif

// TODO(port): not sure why it was here and if it's still needed
#if 0
static void fz_run_page_transparency(fz_context* ctx, Vec<PageAnnotation>& pageAnnots, fz_device* dev,
                                     const fz_rect* cliprect, bool endGroup, bool hasTransparency = false) {
    if (hasTransparency || pageAnnots.size() == 0)
        return;
    bool needsTransparency = false;
    for (size_t i = 0; i < pageAnnots.size(); i++) {
        if (PageAnnotType::Highlight == pageAnnots.at(i).type) {
            needsTransparency = true;
            break;
        }
    }
    if (!needsTransparency)
        return;
    if (!endGroup) {
        fz_rect r = cliprect ? *cliprect : fz_infinite_rect;
        fz_begin_group(ctx, dev, r, nullptr, 1, 0, 0, 1);
    } else
        fz_end_group(ctx, dev);
}
#endif

///// PDF-specific extensions to Fitz/MuPDF /////

extern "C" {
#include <mupdf/pdf.h>
}

namespace str {
namespace conv {

inline WCHAR* FromPdf(fz_context* ctx, pdf_obj* obj) {
    char* s = pdf_new_utf8_from_pdf_string_obj(ctx, obj);
    WCHAR* res = str::conv::FromUtf8(s);
    fz_free(ctx, s);
    return res;
}

} // namespace conv
} // namespace str

// some PDF documents contain control characters in outline titles or /Info properties
WCHAR* pdf_clean_string(WCHAR* string) {
    for (WCHAR* c = string; *c; c++) {
        if (*c < 0x20 && *c != '\n' && *c != '\r' && *c != '\t')
            *c = ' ';
    }
    return string;
}

pdf_obj* pdf_copy_str_dict(fz_context* ctx, pdf_document* doc, pdf_obj* dict) {
    pdf_obj* copy = pdf_copy_dict(ctx, dict);
    for (int i = 0; i < pdf_dict_len(ctx, copy); i++) {
        pdf_obj* val = pdf_dict_get_val(ctx, copy, i);
        // resolve all indirect references
        if (pdf_is_indirect(ctx, val)) {
            auto s = pdf_to_str_buf(ctx, val);
            auto slen = pdf_to_str_len(ctx, val);
            pdf_obj* val2 = pdf_new_string(ctx, s, slen);
            pdf_dict_put(ctx, copy, pdf_dict_get_key(ctx, copy, i), val2);
            pdf_drop_obj(ctx, val2);
        }
    }
    return copy;
}

// Note: make sure to only call with ctxAccess
static fz_outline* pdf_load_attachments(fz_context* ctx, pdf_document* doc) {
    pdf_obj* dict = pdf_load_name_tree(ctx, doc, PDF_NAME(EmbeddedFiles));
    if (!dict)
        return nullptr;

    fz_outline root = {0}, *node = &root;
    for (int i = 0; i < pdf_dict_len(ctx, dict); i++) {
        pdf_obj* name = pdf_dict_get_key(ctx, dict, i);
        pdf_obj* dest = pdf_dict_get_val(ctx, dict, i);
        auto ef = pdf_dict_gets(ctx, dest, "EF");
        pdf_obj* embedded = pdf_dict_getsa(ctx, ef, "DOS", "F");
        if (!embedded)
            continue;

        // TODO: in fz_try ?
        char* uri = pdf_parse_file_spec(ctx, doc, dest, nullptr);
        // undo the mangling done in pdf_parse_file_spec
        if (str::StartsWith(uri, "file://")) {
            char* prev = uri;
            uri = fz_strdup(ctx, uri + 7);
            fz_free(ctx, prev);
        }

        char* title = fz_strdup(ctx, pdf_to_name(ctx, name));
        int streamNo = pdf_to_num(ctx, embedded);
        fz_outline* link = fz_new_outline(ctx);

        link->uri = uri;
        link->title = title;
        // TODO: a hack: re-using page as stream number
        link->page = streamNo;

        node = node->next = link;
    }
    pdf_drop_obj(ctx, dict);

    return root.next;
}

struct PageLabelInfo {
    int startAt = 0;
    int countFrom = 0;
    const char* type = nullptr;
    pdf_obj* prefix = nullptr;
};

int CmpPageLabelInfo(const void* a, const void* b) {
    return ((PageLabelInfo*)a)->startAt - ((PageLabelInfo*)b)->startAt;
}

WCHAR* FormatPageLabel(const char* type, int pageNo, const WCHAR* prefix) {
    if (str::Eq(type, "D"))
        return str::Format(L"%s%d", prefix, pageNo);
    if (str::EqI(type, "R")) {
        // roman numbering style
        AutoFreeW number(str::FormatRomanNumeral(pageNo));
        if (*type == 'r')
            str::ToLowerInPlace(number.Get());
        return str::Format(L"%s%s", prefix, number);
    }
    if (str::EqI(type, "A")) {
        // alphabetic numbering style (A..Z, AA..ZZ, AAA..ZZZ, ...)
        str::Str<WCHAR> number;
        number.Append('A' + (pageNo - 1) % 26);
        for (int i = 0; i < (pageNo - 1) / 26; i++)
            number.Append(number.at(0));
        if (*type == 'a')
            str::ToLowerInPlace(number.Get());
        return str::Format(L"%s%s", prefix, number.Get());
    }
    return str::Dup(prefix);
}

void BuildPageLabelRec(fz_context* ctx, pdf_obj* node, int pageCount, Vec<PageLabelInfo>& data) {
    pdf_obj* obj;
    if ((obj = pdf_dict_gets(ctx, node, "Kids")) != nullptr && !pdf_mark_obj(ctx, node)) {
        for (int i = 0; i < pdf_array_len(ctx, obj); i++) {
            auto arr = pdf_array_get(ctx, obj, i);
            BuildPageLabelRec(ctx, arr, pageCount, data);
        }
        pdf_unmark_obj(ctx, node);
    } else if ((obj = pdf_dict_gets(ctx, node, "Nums")) != nullptr) {
        for (int i = 0; i < pdf_array_len(ctx, obj); i += 2) {
            pdf_obj* info = pdf_array_get(ctx, obj, i + 1);
            PageLabelInfo pli;
            pli.startAt = pdf_to_int(ctx, pdf_array_get(ctx, obj, i)) + 1;
            if (pli.startAt < 1)
                continue;

            pli.type = pdf_to_name(ctx, pdf_dict_gets(ctx, info, "S"));
            pli.prefix = pdf_dict_gets(ctx, info, "P");
            pli.countFrom = pdf_to_int(ctx, pdf_dict_gets(ctx, info, "St"));
            if (pli.countFrom < 1)
                pli.countFrom = 1;
            data.Append(pli);
        }
    }
}

WStrVec* BuildPageLabelVec(fz_context* ctx, pdf_obj* root, int pageCount) {
    Vec<PageLabelInfo> data;
    BuildPageLabelRec(ctx, root, pageCount, data);
    data.Sort(CmpPageLabelInfo);

    if (data.size() == 0)
        return nullptr;

    if (data.size() == 1 && data.at(0).startAt == 1 && data.at(0).countFrom == 1 && !data.at(0).prefix &&
        str::Eq(data.at(0).type, "D")) {
        // this is the default case, no need for special treatment
        return nullptr;
    }

    WStrVec* labels = new WStrVec();
    labels->AppendBlanks(pageCount);

    for (size_t i = 0; i < data.size() && data.at(i).startAt <= pageCount; i++) {
        int secLen = pageCount + 1 - data.at(i).startAt;
        if (i < data.size() - 1 && data.at(i + 1).startAt <= pageCount)
            secLen = data.at(i + 1).startAt - data.at(i).startAt;
        AutoFreeW prefix(str::conv::FromPdf(ctx, data.at(i).prefix));
        for (int j = 0; j < secLen; j++) {
            free(labels->at(data.at(i).startAt + j - 1));
            labels->at(data.at(i).startAt + j - 1) = FormatPageLabel(data.at(i).type, data.at(i).countFrom + j, prefix);
        }
    }

    for (int ix = 0; (ix = labels->Find(nullptr, ix)) != -1; ix++)
        labels->at(ix) = str::Dup(L"");

    // ensure that all page labels are unique (by appending a number to duplicates)
    WStrVec dups(*labels);
    dups.Sort();
    for (size_t i = 1; i < dups.size(); i++) {
        if (!str::Eq(dups.at(i), dups.at(i - 1)))
            continue;
        int ix = labels->Find(dups.at(i)), counter = 0;
        while ((ix = labels->Find(dups.at(i), ix + 1)) != -1) {
            AutoFreeW unique;
            do {
                unique.Set(str::Format(L"%s.%d", dups.at(i), ++counter));
            } while (labels->Contains(unique));
            str::ReplacePtr(&labels->at(ix), unique);
        }
        for (; i + 1 < dups.size() && str::Eq(dups.at(i), dups.at(i + 1)); i++)
            ;
    }

    return labels;
}

struct PageTreeStackItem {
    pdf_obj* kids = nullptr;
    int i = -1;
    int len = 0;
    int next_page_no = 0;

    PageTreeStackItem(){};
    explicit PageTreeStackItem(fz_context* ctx, pdf_obj* kids, int next_page_no = 0)
        : kids(kids), i(-1), len(pdf_array_len(ctx, kids)), next_page_no(next_page_no) {
    }
};

///// Above are extensions to Fitz and MuPDF, now follows PdfEngine /////

struct PdfPageInfo {
    int pageNo = 0; // 1-based
    pdf_page* page = nullptr;
    fz_display_list* list = nullptr;
    fz_stext_page* stext = nullptr;
    RectD mediabox = {};
    // array of annotations
    pdf_annot** pageAnnots = nullptr;
    // array of image rects
    fz_rect* imageRects = nullptr;
};

struct PdfPageRun {
    PdfPageInfo* pageInfo = nullptr;
    int refs = 1;

    PdfPageRun(PdfPageInfo*);
};

PdfPageRun::PdfPageRun(PdfPageInfo* pageInfo) {
    this->pageInfo = pageInfo;
}

class PdfTocItem;
class PdfLink;
class PdfImage;

class PdfEngineImpl : public BaseEngine {
  public:
    PdfEngineImpl();
    virtual ~PdfEngineImpl();
    BaseEngine* Clone() override;

    int PageCount() const override;

    RectD PageMediabox(int pageNo) override;
    RectD PageContentBox(int pageNo, RenderTarget target = RenderTarget::View) override;

    RenderedBitmap* RenderBitmap(int pageNo, float zoom, int rotation,
                                 RectD* pageRect = nullptr, /* if nullptr: defaults to the page's mediabox */
                                 RenderTarget target = RenderTarget::View, AbortCookie** cookie_out = nullptr) override;

    PointD Transform(PointD pt, int pageNo, float zoom, int rotation, bool inverse = false) override;
    RectD Transform(RectD rect, int pageNo, float zoom, int rotation, bool inverse = false) override;

    unsigned char* GetFileData(size_t* cbCount) override;
    bool SaveFileAs(const char* copyFileName, bool includeUserAnnots = false) override;
    virtual bool SaveFileAsPdf(const char* pdfFileName, bool includeUserAnnots = false) {
        return SaveFileAs(pdfFileName, includeUserAnnots);
    }
    WCHAR* ExtractPageText(int pageNo, const WCHAR* lineSep, RectI** coordsOut = nullptr,
                           RenderTarget target = RenderTarget::View) override;
    WCHAR* ExtractPageTextFromPageInfo(PdfPageInfo* pageInfo, const WCHAR* lineSep, RectI** coordsOut = nullptr,
                                       RenderTarget target = RenderTarget::View, bool cacheRun = false);
    bool HasClipOptimizations(int pageNo) override;
    PageLayoutType PreferredLayout() override;
    WCHAR* GetProperty(DocumentProperty prop) override;

    bool SupportsAnnotation(bool forSaving = false) const override;
    void UpdateUserAnnotations(Vec<PageAnnotation>* list) override;

    bool AllowsPrinting() const override {
        return pdf_has_permission(ctx, _doc, FZ_PERMISSION_PRINT);
    }
    bool AllowsCopyingText() const override {
        return pdf_has_permission(ctx, _doc, FZ_PERMISSION_COPY);
    }

    float GetFileDPI() const override {
        return 72.0f;
    }
    const WCHAR* GetDefaultFileExt() const override {
        return L".pdf";
    }

    bool BenchLoadPage(int pageNo) override {
        return GetPdfPage(pageNo) != nullptr;
    }

    Vec<PageElement*>* GetElements(int pageNo) override;
    PageElement* GetElementAtPos(int pageNo, PointD pt) override;

    PageDestination* GetNamedDest(const WCHAR* name) override;
    bool HasTocTree() const override {
        return outline != nullptr || attachments != nullptr;
    }
    DocTocTree* GetTocTree() override;

    bool HasPageLabels() const override {
        return _pagelabels != nullptr;
    }
    WCHAR* GetPageLabel(int pageNo) const override;
    int GetPageByLabel(const WCHAR* label) const override;

    bool IsPasswordProtected() const override {
        return isProtected;
    }
    char* GetDecryptionKey() const override;

    static BaseEngine* CreateFromFile(const WCHAR* fileName, PasswordUI* pwdUI);
    static BaseEngine* CreateFromStream(IStream* stream, PasswordUI* pwdUI);

    // make sure to never ask for pagesAccess in an ctxAccess
    // protected critical section in order to avoid deadlocks
    CRITICAL_SECTION ctxAccess;
    CRITICAL_SECTION pagesAccess;

    RenderedBitmap* GetPageImage(int pageNo, RectD rect, size_t imageIx);
    bool SaveEmbedded(LinkSaverUI& saveUI, int num);

  protected:
    char* _decryptionKey = nullptr;
    bool isProtected = false;
    int pageCount = -1;

    fz_context* ctx = nullptr;
    fz_locks_context fz_locks_ctx;
    pdf_document* _doc = nullptr;
    fz_stream* _docStream = nullptr;
    PdfPageInfo* _pages = nullptr;
    fz_outline* outline = nullptr;
    fz_outline* attachments = nullptr;
    pdf_obj* _info = nullptr;       // TODO(port): what is it?
    WStrVec* _pagelabels = nullptr; // TODO(port): put in PageInfo

    Vec<PageAnnotation> userAnnots; // TODO(port): put in PageInfo
    Vec<PdfPageRun*> runCache;      // ordered most recently used first

    bool Load(const WCHAR* fileName, PasswordUI* pwdUI = nullptr);
    bool Load(IStream* stream, PasswordUI* pwdUI = nullptr);
    // TODO(port): fz_stream can no-longer be re-opened (fz_clone_stream)
    // bool Load(fz_stream* stm, PasswordUI* pwdUI = nullptr);
    bool LoadFromStream(fz_stream* stm, PasswordUI* pwdUI = nullptr);
    bool FinishLoading();

    pdf_page* GetPdfPage(int pageNo, bool failIfBusy = false);
    PdfPageInfo* GetPdfPageInfo(int pageNo, bool failIfBusy = false);
    fz_matrix viewctm(int pageNo, float zoom, int rotation) {
        const fz_rect tmpRc = fz_RectD_to_rect(PageMediabox(pageNo));
        return fz_create_view_ctm(tmpRc, zoom, rotation);
    }
    fz_matrix viewctm(pdf_page* page, float zoom, int rotation) {
        return fz_create_view_ctm(pdf_bound_page(ctx, page), zoom, rotation);
    }
    PdfPageRun* CreatePageRun(PdfPageInfo* pageInfo, fz_display_list* list);
    PdfPageRun* GetPageRun(PdfPageInfo* pageInfo, bool tryOnly = false);
    bool RunPage(PdfPageInfo* pageInfo, fz_device* dev, fz_matrix ctm, RenderTarget target = RenderTarget::View,
                 fz_rect cliprect = {}, bool cacheRun = true, FitzAbortCookie* cookie = nullptr);
    void DropPageRun(PdfPageRun* run, bool forceRemove = false);

    PdfTocItem* BuildTocTree(fz_outline* entry, int& idCounter, bool isAttachment);
    void LinkifyPageText(PdfPageInfo* pageInfo);
    pdf_annot** ProcessPageAnnotations(PdfPageInfo* pageInfo);
    WCHAR* ExtractFontList();
    bool IsLinearizedFile();

    bool SaveUserAnnots(const char* fileName);
};

class PdfLink : public PageElement, public PageDestination {
  public:
    PdfEngineImpl* engine = nullptr;
    // must be one or the other
    fz_link* link = nullptr;
    fz_outline* outline = nullptr;
    int pageNo = 0;
    bool isAttachment = false;

    PdfLink(PdfEngineImpl* engine, int pageNo, fz_link* link, fz_outline* outline);

    // PageElement
    PageElementType GetType() const override {
        return PageElementType::Link;
    }
    int GetPageNo() const override {
        return pageNo;
    }
    RectD GetRect() const override;
    WCHAR* GetValue() const override;
    virtual PageDestination* AsLink() {
        return this;
    }

    // PageDestination
    PageDestType GetDestType() const override;
    int GetDestPageNo() const override;
    RectD GetDestRect() const override;
    WCHAR* GetDestValue() const override {
        return GetValue();
    }
    WCHAR* GetDestName() const override;

    virtual bool SaveEmbedded(LinkSaverUI& saveUI);
};

class PdfComment : public PageElement {
  public:
    PageAnnotation annot;
    AutoFreeW content;

    PdfComment(const WCHAR* content, RectD rect, int pageNo)
        : annot(PageAnnotType::None, pageNo, rect, PageAnnotation::Color()), content(str::Dup(content)) {
    }

    virtual PageElementType GetType() const {
        return PageElementType::Comment;
    }
    virtual int GetPageNo() const {
        return annot.pageNo;
    }
    virtual RectD GetRect() const {
        return annot.rect;
    }
    virtual WCHAR* GetValue() const {
        return str::Dup(content);
    }
};

class PdfTocItem : public DocTocItem {
  public:
    PdfLink link;

    PdfTocItem(WCHAR* title, PdfLink link) : DocTocItem(title), link(link) {
    }

    PageDestination* GetLink() override {
        return &link;
    }
};

class PdfImage : public PageElement {
  public:
    PdfEngineImpl* engine;
    int pageNo;
    RectD rect;
    size_t imageIdx;

    PdfImage(PdfEngineImpl* engine, int pageNo, fz_rect rect, size_t imageIdx)
        : engine(engine), pageNo(pageNo), rect(fz_rect_to_RectD(rect)), imageIdx(imageIdx) {
    }

    virtual PageElementType GetType() const {
        return PageElementType::Image;
    }
    virtual int GetPageNo() const {
        return pageNo;
    }
    virtual RectD GetRect() const {
        return rect;
    }
    virtual WCHAR* GetValue() const {
        return nullptr;
    }

    virtual RenderedBitmap* GetImage() {
        return engine->GetPageImage(pageNo, rect, imageIdx);
    }
};

// in mupdf_load_system_font.c
extern "C" void pdf_install_load_system_font_funcs(fz_context* ctx);

// TODO(port): improve locking to use lock
extern "C" static void fz_lock_context_cs(void* user, int lock) {
    UNUSED(lock);
    PdfEngineImpl* e = (PdfEngineImpl*)user;
    // we use a single critical section for all locks,
    // since that critical section (ctxAccess) should
    // be guarding all fz_context access anyway and
    // thus already be in place (in debug builds we
    // crash if that assertion doesn't hold)
    BOOL ok = TryEnterCriticalSection(&e->ctxAccess);
    if (!ok) {
        CrashIf(true);
        EnterCriticalSection(&e->ctxAccess);
    }
}

extern "C" static void fz_unlock_context_cs(void* user, int lock) {
    UNUSED(lock);
    PdfEngineImpl* e = (PdfEngineImpl*)user;
    LeaveCriticalSection(&e->ctxAccess);
}

PdfEngineImpl::PdfEngineImpl() {
    InitializeCriticalSection(&pagesAccess);
    InitializeCriticalSection(&ctxAccess);

    fz_locks_ctx.user = this;
    fz_locks_ctx.lock = fz_lock_context_cs;
    fz_locks_ctx.unlock = fz_unlock_context_cs;
    ctx = fz_new_context(nullptr, &fz_locks_ctx, FZ_STORE_UNLIMITED);

    pdf_install_load_system_font_funcs(ctx);
}

PdfEngineImpl::~PdfEngineImpl() {
    EnterCriticalSection(&pagesAccess);
    EnterCriticalSection(&ctxAccess);

    for (int i = 0; _pages && i < pageCount; i++) {
        PdfPageInfo* pi = &_pages[i];
        free(pi->pageAnnots);
        free(pi->imageRects);
        if (pi->stext) {
            fz_drop_stext_page(ctx, pi->stext);
        }
        if (pi->list) {
            fz_drop_display_list(ctx, pi->list);
        }
        if (pi->page) {
            fz_drop_page(ctx, (fz_page*)pi->page);
        }
    }

    free(_pages);
    fz_drop_outline(ctx, outline);
    fz_drop_outline(ctx, attachments);
    pdf_drop_obj(ctx, _info);

    while (runCache.size() > 0) {
        AssertCrash(runCache.Last()->refs == 1);
        DropPageRun(runCache.Last(), true);
    }

    // fz_drop_stream(ctx, _docStream);
    pdf_drop_document(ctx, _doc);
    _doc = nullptr;
    fz_drop_context(ctx);
    ctx = nullptr;

    delete _pagelabels;
    free(_decryptionKey);

    LeaveCriticalSection(&ctxAccess);
    DeleteCriticalSection(&ctxAccess);
    LeaveCriticalSection(&pagesAccess);
    DeleteCriticalSection(&pagesAccess);
}

int PdfEngineImpl::PageCount() const {
    CrashIf(pageCount < 0);
    return pageCount;
}

class PasswordCloner : public PasswordUI {
    unsigned char* cryptKey;

  public:
    explicit PasswordCloner(unsigned char* cryptKey) : cryptKey(cryptKey) {
    }

    virtual WCHAR* GetPassword(const WCHAR* fileName, unsigned char* fileDigest, unsigned char decryptionKeyOut[32],
                               bool* saveKey) {
        UNUSED(fileName);
        UNUSED(fileDigest);
        memcpy(decryptionKeyOut, cryptKey, 32);
        *saveKey = true;
        return nullptr;
    }
};

BaseEngine* PdfEngineImpl::Clone() {
    ScopedCritSec scope(&ctxAccess);

    // use this document's encryption key (if any) to load the clone
    PasswordCloner* pwdUI = nullptr;
    if (pdf_crypt_key(ctx, _doc->crypt))
        pwdUI = new PasswordCloner(pdf_crypt_key(ctx, _doc->crypt));

    PdfEngineImpl* clone = new PdfEngineImpl();
    bool ok = false;
    if (FileName()) {
        ok = clone->Load(FileName(), pwdUI);
    } else {
        CrashMe();
        // TODO(port): might not bepossible
        // ok = clone->Load(_doc->file, pwdUI);
    }
    if (!ok) {
        delete clone;
        delete pwdUI;
        return nullptr;
    }
    delete pwdUI;

    if (!_decryptionKey && _doc->crypt) {
        delete clone->_decryptionKey;
        clone->_decryptionKey = nullptr;
    }

    clone->UpdateUserAnnotations(&userAnnots);

    return clone;
}

static const WCHAR* findEmbedMarks(const WCHAR* fileName) {
    const WCHAR* embedMarks = nullptr;

    int colonCount = 0;
    for (const WCHAR* c = fileName + str::Len(fileName) - 1; c > fileName; c--) {
        if (*c == ':') {
            if (!str::IsDigit(*(c + 1)))
                break;
            if (++colonCount % 2 == 0)
                embedMarks = c;
        } else if (!str::IsDigit(*c))
            break;
    }

    return embedMarks;
}

bool PdfEngineImpl::Load(const WCHAR* fileName, PasswordUI* pwdUI) {
    AssertCrash(!FileName() && !_doc && ctx);
    SetFileName(fileName);
    if (!ctx)
        return false;

    fz_stream* file = nullptr;
    // File names ending in :<digits>:<digits> are interpreted as containing
    // embedded PDF documents (the digits are :<num>:<gen> of the embedded file stream)
    WCHAR* embedMarks = (WCHAR*)findEmbedMarks(fileName);
    if (embedMarks)
        *embedMarks = '\0';
    fz_try(ctx) {
        file = fz_open_file2(ctx, fileName);
    }
    fz_catch(ctx) {
        file = nullptr;
    }
    if (embedMarks)
        *embedMarks = ':';

OpenEmbeddedFile:
    if (!LoadFromStream(file, pwdUI))
        return false;

    if (str::IsEmpty(embedMarks))
        return FinishLoading();

    int num, gen;
    embedMarks = (WCHAR*)str::Parse(embedMarks, L":%d:%d", &num, &gen);
    CrashIf(!embedMarks);
    // TODO(port): not sure if this fully translates as gen is no longer used
    if (!embedMarks || !pdf_obj_num_is_stream(ctx, _doc, num))
        return false;

    fz_buffer* buffer = nullptr;
    fz_var(buffer);
    fz_try(ctx) {
        // TODO(port): not sure if this is the right translation
        CrashMe();
        buffer = pdf_load_stream_number(ctx, _doc, num);
        file = fz_open_buffer(ctx, buffer);
    }
    fz_always(ctx) {
        fz_drop_buffer(ctx, buffer);
    }
    fz_catch(ctx) {
        return false;
    }

    pdf_drop_document(ctx, _doc);
    _doc = nullptr;

    goto OpenEmbeddedFile;
}

bool PdfEngineImpl::Load(IStream* stream, PasswordUI* pwdUI) {
    AssertCrash(!FileName() && !_doc && ctx);
    if (!ctx)
        return false;

    fz_stream* stm = nullptr;
    fz_try(ctx) {
        stm = fz_open_istream(ctx, stream);
    }
    fz_catch(ctx) {
        return false;
    }
    if (!LoadFromStream(stm, pwdUI))
        return false;
    return FinishLoading();
}

// TODO(port): fz_stream can't be re-opened anymore
#if 0
bool PdfEngineImpl::Load(fz_strem *stm, PasswordUI* pwdUI) {
    auto fn = FileName();
    AssertCrash(!fn && !_doc && ctx);
    if (!ctx)
        return false;

    fz_stream* stm = nullptr;
    fz_try(ctx) { stm = fz_open_file_w(ctx, File); }
    fz_catch(ctx) { return false; }
    if (!LoadFromStream(stm, pwdUI))
        return false;
    return FinishLoading();
}
#endif

bool PdfEngineImpl::LoadFromStream(fz_stream* stm, PasswordUI* pwdUI) {
    if (!stm)
        return false;

    fz_try(ctx) {
        _doc = pdf_open_document_with_stream(ctx, stm);
    }
    fz_always(ctx) {
        fz_drop_stream(ctx, stm);
    }
    fz_catch(ctx) {
        return false;
    }

    _docStream = stm;

    isProtected = pdf_needs_password(ctx, _doc);
    if (!isProtected)
        return true;

    if (!pwdUI)
        return false;

    unsigned char digest[16 + 32] = {0};
    fz_stream_fingerprint(ctx, _doc->file, digest);

    bool ok = false, saveKey = false;
    while (!ok) {
        AutoFreeW pwd(pwdUI->GetPassword(FileName(), digest, pdf_crypt_key(ctx, _doc->crypt), &saveKey));
        if (!pwd) {
            // password not given or encryption key has been remembered
            ok = saveKey;
            break;
        }

        // MuPDF expects passwords to be UTF-8 encoded
        OwnedData pwd_utf8(str::conv::ToUtf8(pwd));
        ok = pwd_utf8.Get() && pdf_authenticate_password(ctx, _doc, pwd_utf8.Get());
        // according to the spec (1.7 ExtensionLevel 3), the password
        // for crypt revisions 5 and above are in SASLprep normalization
        if (!ok) {
            // TODO: this is only part of SASLprep
            pwd.Set(NormalizeString(pwd, 5 /* NormalizationKC */));
            if (pwd) {
                pwd_utf8 = std::move(str::conv::ToUtf8(pwd));
                ok = pwd_utf8.Get() && pdf_authenticate_password(ctx, _doc, pwd_utf8.Get());
            }
        }
        // older Acrobat versions seem to have considered passwords to be in codepage 1252
        // note: such passwords aren't portable when stored as Unicode text
        if (!ok && GetACP() != 1252) {
            OwnedData pwd_ansi(str::conv::ToAnsi(pwd));
            AutoFreeW pwd_cp1252(str::conv::FromCodePage(pwd_ansi.Get(), 1252));
            pwd_utf8 = std::move(str::conv::ToUtf8(pwd_cp1252));
            ok = pwd_utf8.Get() && pdf_authenticate_password(ctx, _doc, pwd_utf8.Get());
        }
    }

    if (ok && saveKey) {
        memcpy(digest + 16, pdf_crypt_key(ctx, _doc->crypt), 32);
        _decryptionKey = _MemToHex(&digest);
    }

    return ok;
}

bool PdfEngineImpl::FinishLoading() {
    pageCount = 0;
    fz_try(ctx) {
        // this call might throw the first time
        pageCount = pdf_count_pages(ctx, _doc);
    }
    fz_catch(ctx) {
        return false;
    }
    if (pageCount == 0) {
        fz_warn(ctx, "document has no pages");
        return false;
    }
    _pages = AllocArray<PdfPageInfo>(pageCount);
    CrashAlwaysIf(!_pages);

    ScopedCritSec scope(&ctxAccess);

    // TODO: time how long this takes
    for (int i = 0; i < pageCount; i++) {
        fz_rect mbox;
        fz_matrix page_ctm;

        fz_try(ctx) {
            pdf_obj* pageref = pdf_lookup_page_obj(ctx, _doc, i);
            pdf_page_obj_transform(ctx, pageref, &mbox, &page_ctm);
            mbox = fz_transform_rect(mbox, page_ctm);
        }
        fz_catch(ctx) {
        }
        if (fz_is_empty_rect(mbox)) {
            fz_warn(ctx, "cannot find page size for page %d", i);
            mbox.x0 = 0;
            mbox.y0 = 0;
            mbox.x1 = 612;
            mbox.y1 = 792;
        }

        _pages[i].mediabox = fz_rect_to_RectD(mbox);
        _pages[i].pageNo = i + 1;
    }

    fz_try(ctx) {
        outline = pdf_load_outline(ctx, _doc);
    }
    fz_catch(ctx) {
        // ignore errors from pdf_load_outline()
        // this information is not critical and checking the
        // error might prevent loading some pdfs that would
        // otherwise get displayed
        fz_warn(ctx, "Couldn't load outline");
    }

    fz_try(ctx) {
        attachments = pdf_load_attachments(ctx, _doc);
    }
    fz_catch(ctx) {
        fz_warn(ctx, "Couldn't load attachments");
    }

    fz_try(ctx) {
        // keep a copy of the Info dictionary, as accessing the original
        // isn't thread safe and we don't want to block for this when
        // displaying document properties
        _info = pdf_dict_gets(ctx, pdf_trailer(ctx, _doc), "Info");
        if (_info)
            _info = pdf_copy_str_dict(ctx, _doc, _info);
        if (!_info)
            _info = pdf_new_dict(ctx, _doc, 4);
        // also remember linearization and tagged states at this point
        if (IsLinearizedFile())
            pdf_dict_puts_drop(ctx, _info, "Linearized", PDF_TRUE);
        if (pdf_to_bool(ctx, pdf_dict_getp(ctx, pdf_trailer(ctx, _doc), "Root/MarkInfo/Marked")))
            pdf_dict_puts_drop(ctx, _info, "Marked", PDF_TRUE);
        // also remember known output intents (PDF/X, etc.)
        pdf_obj* intents = pdf_dict_getp(ctx, pdf_trailer(ctx, _doc), "Root/OutputIntents");
        if (pdf_is_array(ctx, intents)) {
            pdf_obj* list = pdf_new_array(ctx, _doc, pdf_array_len(ctx, intents));
            for (int i = 0; i < pdf_array_len(ctx, intents); i++) {
                pdf_obj* intent = pdf_dict_gets(ctx, pdf_array_get(ctx, intents, i), "S");
                if (pdf_is_name(ctx, intent) && !pdf_is_indirect(ctx, intent) &&
                    str::StartsWith(pdf_to_name(ctx, intent), "GTS_PDF"))
                    pdf_array_push(ctx, list, intent);
            }
            pdf_dict_puts_drop(ctx, _info, "OutputIntents", list);
        }
        // also note common unsupported features (such as XFA forms)
        pdf_obj* xfa = pdf_dict_getp(ctx, pdf_trailer(ctx, _doc), "Root/AcroForm/XFA");
        if (pdf_is_array(ctx, xfa))
            pdf_dict_puts_drop(ctx, _info, "Unsupported_XFA", PDF_TRUE);
    }
    fz_catch(ctx) {
        fz_warn(ctx, "Couldn't load document properties");
        pdf_drop_obj(ctx, _info);
        _info = nullptr;
    }

    fz_try(ctx) {
        pdf_obj* pagelabels = pdf_dict_getp(ctx, pdf_trailer(ctx, _doc), "Root/PageLabels");
        if (pagelabels)
            _pagelabels = BuildPageLabelVec(ctx, pagelabels, PageCount());
    }
    fz_catch(ctx) {
        fz_warn(ctx, "Couldn't load page labels");
    }

    // TODO: support javascript
    AssertCrash(!pdf_js_supported(ctx, _doc));

    return true;
}

static TocItemFlags pdfFlagsToTocItemFlags(int flags) {
    TocItemFlags res = TocItemFlags::None;

    // TODO: not sure about the mappings yet
    if (flags & 0x1) {
        res = res | TocItemFlags::Italic;
    }
    if (flags & 0x2) {
        res = res | TocItemFlags::Bold;
    }
    if ((flags & ~0x3) != 0) {
        // TODO: is there more?
        CrashMe();
    }
    return res;
}

static COLORREF pdfColorToCOLORREF(float color[4]) {
    return MkRgb(color[0], color[1], color[2]);
}

PdfTocItem* PdfEngineImpl::BuildTocTree(fz_outline* outline, int& idCounter, bool isAttachment) {
    PdfTocItem* root = nullptr;
    PdfTocItem* curr = nullptr;

    while (outline) {
        WCHAR* name = nullptr;
        if (outline->title) {
            name = str::conv::FromUtf8(outline->title);
            name = pdf_clean_string(name);
        }
        if (!name) {
            name = str::Dup(L"");
        }
        int pageNo = outline->page + 1;
        PdfLink link(this, pageNo, nullptr, outline);
        link.isAttachment = isAttachment;
        PdfTocItem* item = new PdfTocItem(name, link);
        item->isOpenDefault = outline->is_open;
        item->id = ++idCounter;
        if (outline->flags != 0) {
            item->flags = pdfFlagsToTocItemFlags(outline->flags);
        }
        if (outline->has_color) {
            item->color = pdfColorToCOLORREF(outline->color);
        }

        if (outline->down) {
            item->child = BuildTocTree(outline->down, idCounter, isAttachment);
        }

        if (!root) {
            root = item;
            curr = item;
        } else {
            curr->next = item;
            curr = item;
        }

        outline = outline->next;
    }

    return root;
}

DocTocTree* PdfEngineImpl::GetTocTree() {
    int idCounter = 0;

    PdfTocItem* root = nullptr;
    if (outline) {
        root = BuildTocTree(outline, idCounter, false);
    }
    if (!attachments) {
        return new DocTocTree(root);
    }
    PdfTocItem* att = BuildTocTree(attachments, idCounter, true);
    if (!root) {
        return new DocTocTree(att);
    }
    root->AddSibling(att);
    return new DocTocTree(root);
}

PageDestination* PdfEngineImpl::GetNamedDest(const WCHAR* name) {
    ScopedCritSec scope1(&pagesAccess);
    ScopedCritSec scope2(&ctxAccess);

    OwnedData name_utf8(str::conv::ToUtf8(name));
    pdf_obj* dest = nullptr;
    fz_try(ctx) {
        pdf_obj* nameobj = pdf_new_string(ctx, name_utf8.Get(), (int)name_utf8.size);
        dest = pdf_lookup_dest(ctx, _doc, nameobj);
        pdf_drop_obj(ctx, nameobj);
    }
    fz_catch(ctx) {
        return nullptr;
    }

    PageDestination* pageDest = nullptr;
    // TODO(port)
    CrashMe();
    // fz_link_dest ld = {FZ_LINK_NONE, 0};
    char* ld;
    fz_try(ctx) {
        ld = pdf_parse_link_dest(ctx, _doc, dest);
    }
    fz_catch(ctx) {
        return nullptr;
    }

    /*
    if (FZ_LINK_GOTO == ld.kind && ld.ld.gotor.page != -1) {
        // create a SimpleDest because we have to
        // free the fz_link_dest before returning
        PdfLink tmp(this, &ld);
        pageDest = new SimpleDest(tmp.GetDestPageNo(), tmp.GetDestRect());
    }
    */
    fz_free(ctx, ld);

    return pageDest;
}

PdfPageInfo* PdfEngineImpl::GetPdfPageInfo(int pageNo, bool failIfBusy) {
    GetPdfPage(pageNo, failIfBusy);
    return &_pages[pageNo - 1];
}

pdf_page* PdfEngineImpl::GetPdfPage(int pageNo, bool failIfBusy) {
    ScopedCritSec scope(&pagesAccess);

    CrashIf(pageNo < 1 || pageNo > pageCount);
    int pageIdx = pageNo - 1;
    PdfPageInfo* pageInfo = &_pages[pageNo - 1];
    pdf_page* ppage = pageInfo->page;
    // TODO: not sure what failIfBusy is supposed to do
    if (ppage || failIfBusy) {
        return ppage;
    }

    ScopedCritSec ctxScope(&ctxAccess);
    fz_var(ppage);
    fz_try(ctx) {
        ppage = pdf_load_page(ctx, _doc, pageNo - 1);
        pageInfo->page = ppage;
    }
    fz_catch(ctx) {
    }

    fz_rect bounds;
    fz_page* page = (fz_page*)ppage;
    fz_display_list* list = NULL;
    fz_device* dev = NULL;
    fz_cookie cookie = {0};
    fz_var(list);
    fz_var(dev);

    /* TODO: handle try later?
        if (fz_caught(ctx) != FZ_ERROR_TRYLATER) {
            return nullptr;
        }
    */
    fz_try(ctx) {
        bounds = fz_bound_page(ctx, page);
        list = fz_new_display_list(ctx, bounds);
        dev = fz_new_list_device(ctx, list);
        // TODO(port): should this be just fz_run_page_contents?
        fz_run_page(ctx, page, dev, fz_identity, &cookie);
    }
    fz_always(ctx) {
        fz_close_device(ctx, dev);
        fz_drop_device(ctx, dev);
        dev = NULL;
    }
    fz_catch(ctx) {
        fz_drop_display_list(ctx, list);
        // fz_drop_separations(ctx, seps);
    }
    if (!list) {
        return ppage;
    }
    pageInfo->list = list;

    fz_stext_page* page_text = fz_new_stext_page(ctx, bounds);
    fz_device* tdev = fz_new_stext_device(ctx, page_text, NULL);

    tdev = fz_new_stext_device(ctx, page_text, NULL);
    fz_try(ctx) {
        // use an infinite rectangle as bounds (instead of pdf_bound_page) to ensure that
        // the extracted text is consistent between cached runs using a list device and
        // fresh runs (otherwise the list device omits text outside the mediabox bounds)
        fz_run_page(ctx, page, tdev, fz_identity, &cookie);
        fz_close_device(ctx, tdev);
    }
    fz_always(ctx) {
        fz_drop_device(ctx, tdev);
    }
    fz_catch(ctx) {
    }
    pageInfo->stext = page_text;

    // create fz_display_list and get fz_stext_page
    ppage->links = FixupPageLinks(ppage->links);
    AssertCrash(!ppage->links || ppage->links->refs == 1);
    LinkifyPageText(pageInfo);

    pageInfo->pageAnnots = ProcessPageAnnotations(pageInfo);
    return ppage;
}

PdfPageRun* PdfEngineImpl::CreatePageRun(PdfPageInfo* pageInfo, fz_display_list* list) {
    Vec<FitzImagePos> positions;

    // save the image rectangles for this page
    int pageNo = pageInfo->pageNo;
    PdfPageInfo* pi = &_pages[pageNo - 1];
    if (!pi->imageRects && positions.size() > 0) {
        // the list of page image rectangles is terminated with a null-rectangle
        fz_rect* rects = AllocArray<fz_rect>(positions.size() + 1);
        if (rects) {
            for (size_t i = 0; i < positions.size(); i++) {
                rects[i] = positions.at(i).rect;
            }
            pi->imageRects = rects;
        }
    }

    auto pageRun = new PdfPageRun(pageInfo);
    return pageRun;
}

PdfPageRun* PdfEngineImpl::GetPageRun(PdfPageInfo* pageInfo, bool tryOnly) {
    // we failed get display list when loading the page for the first time
    if (!pageInfo->list) {
        return nullptr;
    }

    PdfPageRun* result = nullptr;
    fz_cookie cookie = {0};

    ScopedCritSec scope(&pagesAccess);

    for (size_t i = 0; i < runCache.size(); i++) {
        if (runCache.at(i)->pageInfo == pageInfo) {
            result = runCache.at(i);
            break;
        }
    }
    if (!result && !tryOnly) {
        size_t mem = 0;
        for (size_t i = 0; i < runCache.size(); i++) {
            // drop page runs that take up too much memory due to huge images
            // (except for the very recently used ones)
#if 0
            if (i >= 2 && mem + runCache.at(i)->size_est >= MAX_PAGE_RUN_MEMORY)
                DropPageRun(runCache.at(i--), true);
            else
                mem += runCache.at(i)->size_est;
#endif
        }
        if (runCache.size() >= MAX_PAGE_RUN_CACHE) {
            AssertCrash(runCache.size() == MAX_PAGE_RUN_CACHE);
            DropPageRun(runCache.Last(), true);
        }

        ScopedCritSec scope2(&ctxAccess);

        fz_page* page = (fz_page*)pageInfo->page;

        result = CreatePageRun(pageInfo, pageInfo->list);
        runCache.InsertAt(0, result);
    } else if (result && result != runCache.at(0)) {
        // keep the list Most Recently Used first
        runCache.Remove(result);
        runCache.InsertAt(0, result);
    }

    if (result)
        result->refs++;
    return result;
}

bool PdfEngineImpl::RunPage(PdfPageInfo* pageInfo, fz_device* dev, fz_matrix ctm, RenderTarget target, fz_rect cliprect,
                            bool cacheRun, FitzAbortCookie* cookie) {
    bool ok = true;

    fz_cookie* fzcookie = cookie ? &cookie->cookie : nullptr;

    pdf_page* page = pageInfo->page;
    int pageNo = pageInfo->pageNo;
    if (RenderTarget::View == target) {
        PdfPageRun* run = GetPageRun(pageInfo, !cacheRun);
        CrashIf(!run);
        if (!run) {
            return false;
        }
        EnterCriticalSection(&ctxAccess);
        Vec<PageAnnotation> pageAnnots = fz_get_user_page_annots(userAnnots, pageNo);
        fz_try(ctx) {
            // fz_run_page_transparency(ctx, pageAnnots, dev, cliprect, false, page->transparency);
            fz_run_display_list(ctx, run->pageInfo->list, dev, ctm, cliprect, fzcookie);
            // fz_run_page_transparency(ctx, pageAnnots, dev, cliprect, true, page->transparency);
            // fz_run_user_page_annots(ctx, pageAnnots, dev, ctm, cliprect, fzcookie);
        }
        fz_catch(ctx) {
            ok = false;
        }
        LeaveCriticalSection(&ctxAccess);
        DropPageRun(run);
    } else {
        ScopedCritSec scope(&ctxAccess);
        char* targetName = target == RenderTarget::Print ? "Print" : target == RenderTarget::Export ? "Export" : "View";
        Vec<PageAnnotation> pageAnnots = fz_get_user_page_annots(userAnnots, pageNo);
        fz_try(ctx) {
            // TODO(port): not sure if this is the right port
            fz_buffer* buf = fz_new_buffer(ctx, 1024);
            fz_output* out = fz_new_output_with_buffer(ctx, buf);
            auto wri = fz_new_pdf_writer_with_output(ctx, out, nullptr);
            auto pageBounds = pdf_bound_page(ctx, page);
            fz_begin_page(ctx, wri, pageBounds);
            // fz_run_page_transparency(ctx, pageAnnots, dev, cliprect, false, page->transparency);
            pdf_run_page_with_usage(ctx, _doc, page, dev, ctm, targetName, fzcookie);
            // fz_run_page_transparency(ctx, pageAnnots, dev, cliprect, true, page->transparency);
            // fz_run_user_page_annots(ctx, pageAnnots, dev, ctm, cliprect, fzcookie);
            fz_end_page(ctx, wri);
        }
        fz_catch(ctx) {
            ok = false;
        }
    }

    return ok && !(cookie && cookie->cookie.abort);
}

void PdfEngineImpl::DropPageRun(PdfPageRun* run, bool forceRemove) {
    ScopedCritSec scope(&pagesAccess);
    run->refs--;

    if (0 == run->refs || forceRemove)
        runCache.Remove(run);

    if (0 == run->refs) {
        ScopedCritSec ctxScope(&ctxAccess);
        // TODO(port): probably remove as we no longer create a list per page run
        // fz_drop_display_list(ctx, run->pageInfo->list);
        delete run;
    }
}

RectD PdfEngineImpl::PageMediabox(int pageNo) {
    PdfPageInfo* pi = &_pages[pageNo - 1];
    return pi->mediabox;
}

RectD PdfEngineImpl::PageContentBox(int pageNo, RenderTarget target) {
    PdfPageInfo* pi = GetPdfPageInfo(pageNo);

    ScopedCritSec scope(&ctxAccess);

    fz_rect rect = fz_empty_rect;
    fz_device* dev = nullptr;

    dev = fz_new_bbox_device(ctx, &rect);
    fz_rect pagerect = pdf_bound_page(ctx, pi->page);
    bool ok = RunPage(pi, dev, fz_identity, target, pagerect, false);
    fz_drop_device(ctx, dev);
    if (!ok)
        return PageMediabox(pageNo);
    if (fz_is_infinite_rect(rect))
        return PageMediabox(pageNo);

    RectD rect2 = fz_rect_to_RectD(rect);
    return rect2.Intersect(PageMediabox(pageNo));
}

PointD PdfEngineImpl::Transform(PointD pt, int pageNo, float zoom, int rotation, bool inverse) {
    fz_matrix ctm = viewctm(pageNo, zoom, rotation);
    if (inverse)
        ctm = fz_invert_matrix(ctm);
    fz_point pt2 = {(float)pt.x, (float)pt.y};
    pt2 = fz_transform_point(pt2, ctm);
    return PointD(pt2.x, pt2.y);
}

RectD PdfEngineImpl::Transform(RectD rect, int pageNo, float zoom, int rotation, bool inverse) {
    fz_matrix ctm = viewctm(pageNo, zoom, rotation);
    if (inverse)
        ctm = fz_invert_matrix(ctm);
    fz_rect rect2 = fz_RectD_to_rect(rect);
    rect2 = fz_transform_rect(rect2, ctm);
    return fz_rect_to_RectD(rect2);
}

RenderedBitmap* PdfEngineImpl::RenderBitmap(int pageNo, float zoom, int rotation, RectD* pageRect, RenderTarget target,
                                            AbortCookie** cookie_out) {
    PdfPageInfo* pageInfo = GetPdfPageInfo(pageNo);
    pdf_page* page = pageInfo->page;

    if (!page) {
        return nullptr;
    }

    fz_cookie* fzcookie = nullptr;
    FitzAbortCookie* cookie = nullptr;
    if (cookie_out) {
        cookie = new FitzAbortCookie();
        *cookie_out = cookie;
        fzcookie = &cookie->cookie;
    }

    // TODO(port): I don't see why this lock is needed
    EnterCriticalSection(&ctxAccess);

    fz_rect pRect;
    if (pageRect) {
        pRect = fz_RectD_to_rect(*pageRect);
    } else {
        // TODO(port): use pageInfo->mediabox?
        pRect = pdf_bound_page(ctx, page);
    }
    fz_matrix ctm = viewctm(page, zoom, rotation);
    fz_irect bbox = fz_round_rect(fz_transform_rect(pRect, ctm));

    fz_colorspace* colorspace = fz_device_rgb(ctx);
    fz_irect ibounds = bbox;
    fz_rect cliprect = fz_rect_from_irect(bbox);

    fz_pixmap* pix = fz_new_pixmap_with_bbox(ctx, colorspace, ibounds, nullptr, 1);
    // initialize white background
    fz_clear_pixmap_with_value(ctx, pix, 0xff);

    fz_device* dev = NULL;
    fz_var(dev);
    fz_try(ctx) {
        // TODO: in printing different style. old code use pdf_run_page_with_usage(), with usage ="View"
        // or "Print". "Export" is not used
        dev = fz_new_draw_device(ctx, fz_identity, pix);
        // TODO: use fz_infinite_rect instead of cliprect?
        fz_run_display_list(ctx, pageInfo->list, dev, ctm, cliprect, fzcookie);
        fz_close_device(ctx, dev);
    }
    fz_always(ctx) {
        fz_drop_device(ctx, dev);
        LeaveCriticalSection(&ctxAccess);
    }
    fz_catch(ctx) {
        fz_drop_pixmap(ctx, pix);
        return nullptr;
    }

    RenderedBitmap* bitmap = new_rendered_fz_pixmap(ctx, pix);
    fz_drop_pixmap(ctx, pix);
    return bitmap;
}

PageElement* PdfEngineImpl::GetElementAtPos(int pageNo, PointD pt) {
    pdf_page* page = GetPdfPage(pageNo, true);
    if (!page) {
        return nullptr;
    }

    fz_link* link = page->links;
    fz_point p = {(float)pt.x, (float)pt.y};
    while (link) {
        if (fz_is_pt_in_rect(link->rect, p)) {
            return new PdfLink(this, pageNo, link, nullptr);
        }
        link = link->next;
    }

    PdfPageInfo* pi = &_pages[pageNo - 1];
    fz_rect* ir = pi->imageRects;
    for (size_t i = 0; ir && !fz_is_empty_rect(ir[i]); i++) {
        if (fz_is_pt_in_rect(pi->imageRects[i], p)) {
            return new PdfImage(this, pageNo, ir[i], i);
        }
    }

#if 0
    if (pageAnnots[pageNo - 1]) {
        for (size_t i = 0; pageAnnots[pageNo - 1][i]; i++) {
            pdf_annot* annot = pageAnnots[pageNo - 1][i];
            fz_rect rect = annot->rect;
            fz_transform_rect(&rect, &page->ctm);
            if (fz_is_pt_in_rect(rect, p)) {
                ScopedCritSec scope(&ctxAccess);

                AutoFreeW contents(str::conv::FromPdf(pdf_dict_gets(annot->obj, "Contents")));
                // TODO: use separate classes for comments and tooltips?
                if (str::IsEmpty(contents.Get()) && FZ_ANNOT_WIDGET == annot->annot_type)
                    contents.Set(str::conv::FromPdf(pdf_dict_gets(annot->obj, "TU")));
                return new PdfComment(contents, fz_rect_to_RectD(rect), pageNo);
            }
        }
    }

#endif

    return nullptr;
}

Vec<PageElement*>* PdfEngineImpl::GetElements(int pageNo) {
    pdf_page* page = GetPdfPage(pageNo, true);
    if (!page)
        return nullptr;
    PdfPageInfo* pi = &_pages[pageNo - 1];

    // since all elements lists are in last-to-first order, append
    // item types in inverse order and reverse the whole list at the end
    Vec<PageElement*>* els = new Vec<PageElement*>();

    fz_rect* ir = pi->imageRects;
    if (ir != nullptr) {
        for (size_t i = 0; !fz_is_empty_rect(ir[i]); i++) {
            els->Append(new PdfImage(this, pageNo, ir[i], i));
        }
    }

#if 0
    // TODO(annots)
    if (pageAnnots[pageNo - 1]) {
        ScopedCritSec scope(&ctxAccess);

        for (size_t i = 0; pageAnnots[pageNo - 1][i]; i++) {
            pdf_annot* annot = pageAnnots[pageNo - 1][i];
            fz_rect rect = annot->rect;
            fz_transform_rect(&rect, &page->ctm);
            AutoFreeW contents(str::conv::FromPdf(pdf_dict_gets(annot->obj, "Contents")));
            if (str::IsEmpty(contents.Get()) && FZ_ANNOT_WIDGET == annot->annot_type)
                contents.Set(str::conv::FromPdf(pdf_dict_gets(annot->obj, "TU")));
            els->Append(new PdfComment(contents, fz_rect_to_RectD(rect), pageNo));
        }
    }

#endif
    fz_link* link = page->links;
    while (link) {
        auto* el = new PdfLink(this, pageNo, link, nullptr);
        els->Append(el);
        link = link->next;
    }

    els->Reverse();
    return els;
}

void PdfEngineImpl::LinkifyPageText(PdfPageInfo* pageInfo) {
    RectI* coords;
    AutoFreeW pageText(ExtractPageTextFromPageInfo(pageInfo, L"\n", &coords, RenderTarget::View, true));
    if (!pageText)
        return;

        // TODO(port)
#if 0
    CrashMePort();
    LinkRectList* list = LinkifyText(pageText, coords);
    for (size_t i = 0; i < list->links.size(); i++) {
        bool overlaps = false;
        for (fz_link* next = page->links; next && !overlaps; next = next->next)
            overlaps = fz_calc_overlap(list->coords.at(i), next->rect) >= 0.25f;
        if (!overlaps) {
            OwnedData uri(str::conv::ToUtf8(list->links.at(i)));
            if (!uri.Get()) {
                continue;
            }
            fz_link_dest ld = {FZ_LINK_URI, 0};
            ld.ld.uri.uri = fz_strdup(ctx, uri.Get());
            // add links in top-to-bottom order (i.e. last-to-first)
            fz_link* link = fz_new_link(ctx, &list->coords.at(i), ld);
            CrashIf(!link); // TODO: if fz_new_link throws, there are memory leaks
            link->next = page->links;
            page->links = link;
        }
    }
    delete list;
#endif
    free(coords);
}

pdf_annot** PdfEngineImpl::ProcessPageAnnotations(PdfPageInfo* pageInfo) {
    Vec<pdf_annot*> annots;

    // TODO(annots)
#if 0
    for (pdf_annot* annot = page->annots; annot; annot = annot->next) {
        if (FZ_ANNOT_FILEATTACHMENT == annot->annot_type) {
            pdf_obj* file = pdf_dict_gets(annot->obj, "FS");
            pdf_obj* embedded = pdf_dict_getsa(pdf_dict_gets(file, "EF"), "DOS", "F");
            fz_rect rect;
            pdf_to_rect(ctx, pdf_dict_gets(annot->obj, "Rect"), &rect);
            if (file && embedded && !fz_is_empty_rect(rect)) {
                fz_link_dest ld;
                ld.kind = FZ_LINK_LAUNCH;
                ld.ld.launch.file_spec = pdf_file_spec_to_str(_doc, file);
                ld.ld.launch.new_window = 1;
                ld.ld.launch.embedded_num = pdf_to_num(embedded);
                ld.ld.launch.embedded_gen = pdf_to_gen(embedded);
                ld.ld.launch.is_uri = 0;
                fz_transform_rect(&rect, &page->ctm);
                // add links in top-to-bottom order (i.e. last-to-first)
                fz_link* link = fz_new_link(ctx, &rect, ld);
                link->next = page->links;
                page->links = link;
                // TODO: expose /Contents in addition to the file path
            } else if (!str::IsEmpty(pdf_to_str_buf(pdf_dict_gets(annot->obj, "Contents")))) {
                annots.Append(annot);
            }
        } else if (!str::IsEmpty(pdf_to_str_buf(pdf_dict_gets(annot->obj, "Contents"))) &&
                   annot->annot_type != FZ_ANNOT_FREETEXT) {
            annots.Append(annot);
        } else if (FZ_ANNOT_WIDGET == annot->annot_type &&
                   !str::IsEmpty(pdf_to_str_buf(pdf_dict_gets(annot->obj, "TU")))) {
            if (!(pdf_to_int(pdf_dict_gets(annot->obj, "Ff")) & Ff_ReadOnly))
                annots.Append(annot);
        }
    }
#endif
    if (annots.size() == 0)
        return nullptr;

    // re-order list into top-to-bottom order (i.e. last-to-first)
    annots.Reverse();
    // add sentinel value
    annots.Append(nullptr);
    return annots.StealData();
}

RenderedBitmap* PdfEngineImpl::GetPageImage(int pageNo, RectD rect, size_t imageIdx) {
    PdfPageInfo* pageInfo = GetPdfPageInfo(pageNo);
    if (!pageInfo->page) {
        return nullptr;
    }

    Vec<FitzImagePos> positions;

    if (imageIdx >= positions.size() || fz_rect_to_RectD(positions.at(imageIdx).rect) != rect) {
        AssertCrash(0);
        return nullptr;
    }

    ScopedCritSec scope(&ctxAccess);

    fz_pixmap* pixmap = nullptr;
    fz_try(ctx) {
        fz_image* image = positions.at(imageIdx).image;
        CrashMePort();
        // TODO(port): not sure if should provide subarea, w and h
        pixmap = fz_get_pixmap_from_image(ctx, image, nullptr, nullptr, nullptr, nullptr);
    }
    fz_catch(ctx) {
        return nullptr;
    }
    RenderedBitmap* bmp = new_rendered_fz_pixmap(ctx, pixmap);
    fz_drop_pixmap(ctx, pixmap);

    return bmp;
}

WCHAR* PdfEngineImpl::ExtractPageText(int pageNo, const WCHAR* lineSep, RectI** coordsOut, RenderTarget target) {
    PdfPageInfo* pageInfo = GetPdfPageInfo(pageNo);
    if (!pageInfo->page) {
        return nullptr;
    }
    return ExtractPageTextFromPageInfo(pageInfo, lineSep, coordsOut, target, false);
}

WCHAR* PdfEngineImpl::ExtractPageTextFromPageInfo(PdfPageInfo* pageInfo, const WCHAR* lineSep, RectI** coordsOut,
                                                  RenderTarget target, bool cacheRun) {
    WCHAR* content = nullptr;
    ScopedCritSec scope(&ctxAccess);
    content = fz_text_page_to_str(pageInfo->stext, lineSep, coordsOut);
    return content;
}

bool PdfEngineImpl::IsLinearizedFile() {
    ScopedCritSec scope(&ctxAccess);
    // determine the object number of the very first object in the file
    fz_seek(ctx, _doc->file, 0, 0);
    int tok = pdf_lex(ctx, _doc->file, &_doc->lexbuf.base);
    if (tok != PDF_TOK_INT)
        return false;
    int num = _doc->lexbuf.base.i;
    if (num < 0 || num >= pdf_xref_len(ctx, _doc))
        return false;
    // check whether it's a linearization dictionary
    fz_try(ctx) {
        pdf_cache_object(ctx, _doc, num);
    }
    fz_catch(ctx) {
        return false;
    }
    pdf_obj* obj = pdf_get_xref_entry(ctx, _doc, num)->obj;
    if (!pdf_is_dict(ctx, obj))
        return false;
    // /Linearized format must be version 1.0
    if (pdf_to_real(ctx, pdf_dict_gets(ctx, obj, "Linearized")) != 1.0f)
        return false;
    // /L must be the exact file size
    if (pdf_to_int(ctx, pdf_dict_gets(ctx, obj, "L")) != _doc->file_size)
        return false;

    // /O must be the object number of the first page
    // TODO(port): at this point we don't have _pages loaded yet. for now always return false here
    auto page = _pages[0].page;
    if (!page) {
        return false;
    }
    if (pdf_to_int(ctx, pdf_dict_gets(ctx, obj, "O")) != pdf_to_num(ctx, page->obj))
        return false;

    // /N must be the total number of pages
    if (pdf_to_int(ctx, pdf_dict_gets(ctx, obj, "N")) != PageCount())
        return false;
    // /H must be an array and /E and /T must be integers
    return pdf_is_array(ctx, pdf_dict_gets(ctx, obj, "H")) && pdf_is_int(ctx, pdf_dict_gets(ctx, obj, "E")) &&
           pdf_is_int(ctx, pdf_dict_gets(ctx, obj, "T"));
}

static void pdf_extract_fonts(fz_context* ctx, pdf_obj* res, Vec<pdf_obj*>& fontList, Vec<pdf_obj*>& resList) {
    if (!res || pdf_mark_obj(ctx, res))
        return;
    resList.Append(res);

    pdf_obj* fonts = pdf_dict_gets(ctx, res, "Font");
    for (int k = 0; k < pdf_dict_len(ctx, fonts); k++) {
        pdf_obj* font = pdf_resolve_indirect(ctx, pdf_dict_get_val(ctx, fonts, k));
        if (font && !fontList.Contains(font))
            fontList.Append(font);
    }
    // also extract fonts for all XObjects (recursively)
    pdf_obj* xobjs = pdf_dict_gets(ctx, res, "XObject");
    for (int k = 0; k < pdf_dict_len(ctx, xobjs); k++) {
        pdf_obj* xobj = pdf_dict_get_val(ctx, xobjs, k);
        pdf_obj* xres = pdf_dict_gets(ctx, xobj, "Resources");
        pdf_extract_fonts(ctx, xres, fontList, resList);
    }
}

WCHAR* PdfEngineImpl::ExtractFontList() {
    Vec<pdf_obj*> fontList;
    Vec<pdf_obj*> resList;

    // collect all fonts from all page objects
    for (int i = 1; i <= PageCount(); i++) {
        pdf_page* page = GetPdfPage(i);
        if (page) {
            ScopedCritSec scope(&ctxAccess);
            fz_try(ctx) {
                pdf_obj* resources = pdf_page_resources(ctx, page);
                pdf_extract_fonts(ctx, resources, fontList, resList);
                for (pdf_annot* annot = page->annots; annot; annot = annot->next) {
                    if (annot->ap) {
                        pdf_obj* o = annot->ap;
                        CrashMePort();
                        // TODO(port): not sure this is the right thing
                        resources = pdf_xobject_resources(ctx, o);
                        pdf_extract_fonts(ctx, resources, fontList, resList);
                    }
                }
            }
            fz_catch(ctx) {
            }
        }
    }

    // start ctxAccess scope here so that we don't also have to
    // ask for pagesAccess (as is required for GetPdfPage)
    ScopedCritSec scope(&ctxAccess);

    for (pdf_obj* res : resList) {
        pdf_unmark_obj(ctx, res);
    }

    WStrVec fonts;
    for (size_t i = 0; i < fontList.size(); i++) {
        const char *name = nullptr, *type = nullptr, *encoding = nullptr;
        AutoFree anonFontName;
        bool embedded = false;
        fz_try(ctx) {
            pdf_obj* font = fontList.at(i);
            pdf_obj* font2 = pdf_array_get(ctx, pdf_dict_gets(ctx, font, "DescendantFonts"), 0);
            if (!font2)
                font2 = font;

            name = pdf_to_name(ctx, pdf_dict_getsa(ctx, font2, "BaseFont", "Name"));
            bool needAnonName = str::IsEmpty(name);
            if (needAnonName && font2 != font) {
                name = pdf_to_name(ctx, pdf_dict_getsa(ctx, font, "BaseFont", "Name"));
                needAnonName = str::IsEmpty(name);
            }
            if (needAnonName) {
                anonFontName.Set(str::Format("<#%d>", pdf_obj_parent_num(ctx, font2)));
                name = anonFontName;
            }
            embedded = false;
            pdf_obj* desc = pdf_dict_gets(ctx, font2, "FontDescriptor");
            if (desc && (pdf_dict_gets(ctx, desc, "FontFile") || pdf_dict_getsa(ctx, desc, "FontFile2", "FontFile3")))
                embedded = true;
            if (embedded && str::Len(name) > 7 && name[6] == '+')
                name += 7;

            type = pdf_to_name(ctx, pdf_dict_gets(ctx, font, "Subtype"));
            if (font2 != font) {
                const char* type2 = pdf_to_name(ctx, pdf_dict_gets(ctx, font2, "Subtype"));
                if (str::Eq(type2, "CIDFontType0"))
                    type = "Type1 (CID)";
                else if (str::Eq(type2, "CIDFontType2"))
                    type = "TrueType (CID)";
            }
            if (str::Eq(type, "Type3"))
                embedded = pdf_dict_gets(ctx, font2, "CharProcs") != nullptr;

            encoding = pdf_to_name(ctx, pdf_dict_gets(ctx, font, "Encoding"));
            if (str::Eq(encoding, "WinAnsiEncoding"))
                encoding = "Ansi";
            else if (str::Eq(encoding, "MacRomanEncoding"))
                encoding = "Roman";
            else if (str::Eq(encoding, "MacExpertEncoding"))
                encoding = "Expert";
        }
        fz_catch(ctx) {
            continue;
        }
        CrashIf(!name || !type || !encoding);

        str::Str<char> info;
        if (name[0] < 0 && MultiByteToWideChar(936, MB_ERR_INVALID_CHARS, name, -1, nullptr, 0))
            info.Append(str::ToMultiByte(name, 936, CP_UTF8).StealData());
        else
            info.Append(name);
        if (!str::IsEmpty(encoding) || !str::IsEmpty(type) || embedded) {
            info.Append(" (");
            if (!str::IsEmpty(type))
                info.AppendFmt("%s; ", type);
            if (!str::IsEmpty(encoding))
                info.AppendFmt("%s; ", encoding);
            if (embedded)
                info.Append("embedded; ");
            info.RemoveAt(info.size() - 2, 2);
            info.Append(")");
        }

        AutoFreeW fontInfo(str::conv::FromUtf8(info.LendData()));
        if (fontInfo && !fonts.Contains(fontInfo))
            fonts.Append(fontInfo.StealData());
    }
    if (fonts.size() == 0)
        return nullptr;

    fonts.SortNatural();
    return fonts.Join(L"\n");
}

WCHAR* PdfEngineImpl::GetProperty(DocumentProperty prop) {
    if (!_doc)
        return nullptr;

    if (DocumentProperty::PdfVersion == prop) {
        int major = _doc->version / 10, minor = _doc->version % 10;
        pdf_crypt* crypt = _doc->crypt;
        if (1 == major && 7 == minor && pdf_crypt_version(ctx, crypt) == 5) {
            if (pdf_crypt_revision(ctx, crypt) == 5)
                return str::Format(L"%d.%d Adobe Extension Level %d", major, minor, 3);
            if (pdf_crypt_revision(ctx, crypt) == 6)
                return str::Format(L"%d.%d Adobe Extension Level %d", major, minor, 8);
        }
        return str::Format(L"%d.%d", major, minor);
    }

    if (DocumentProperty::PdfFileStructure == prop) {
        WStrVec fstruct;
        if (pdf_to_bool(ctx, pdf_dict_gets(ctx, _info, "Linearized")))
            fstruct.Append(str::Dup(L"linearized"));
        if (pdf_to_bool(ctx, pdf_dict_gets(ctx, _info, "Marked")))
            fstruct.Append(str::Dup(L"tagged"));
        if (pdf_dict_gets(ctx, _info, "OutputIntents")) {
            for (int i = 0; i < pdf_array_len(ctx, pdf_dict_gets(ctx, _info, "OutputIntents")); i++) {
                pdf_obj* intent = pdf_array_get(ctx, pdf_dict_gets(ctx, _info, "OutputIntents"), i);
                CrashIf(!str::StartsWith(pdf_to_name(ctx, intent), "GTS_"));
                fstruct.Append(str::conv::FromUtf8(pdf_to_name(ctx, intent) + 4));
            }
        }
        return fstruct.size() > 0 ? fstruct.Join(L",") : nullptr;
    }

    if (DocumentProperty::UnsupportedFeatures == prop) {
        if (pdf_to_bool(ctx, pdf_dict_gets(ctx, _info, "Unsupported_XFA")))
            return str::Dup(L"XFA");
        return nullptr;
    }

    if (DocumentProperty::FontList == prop)
        return ExtractFontList();

    static struct {
        DocumentProperty prop;
        const char* name;
    } pdfPropNames[] = {
        {DocumentProperty::Title, "Title"},
        {DocumentProperty::Author, "Author"},
        {DocumentProperty::Subject, "Subject"},
        {DocumentProperty::Copyright, "Copyright"},
        {DocumentProperty::CreationDate, "CreationDate"},
        {DocumentProperty::ModificationDate, "ModDate"},
        {DocumentProperty::CreatorApp, "Creator"},
        {DocumentProperty::PdfProducer, "Producer"},
    };
    for (int i = 0; i < dimof(pdfPropNames); i++) {
        if (pdfPropNames[i].prop == prop) {
            // _info is guaranteed not to contain any indirect references,
            // so no need for ctxAccess
            pdf_obj* obj = pdf_dict_gets(ctx, _info, pdfPropNames[i].name);
            return obj ? pdf_clean_string(str::conv::FromPdf(ctx, obj)) : nullptr;
        }
    }
    return nullptr;
};

bool PdfEngineImpl::SupportsAnnotation(bool forSaving) const {
    if (forSaving) {
        // TODO: support updating of documents where pages aren't all numbered objects?
        for (int i = 0; i < PageCount(); i++) {
            PdfPageInfo* pi = &_pages[i];
            pdf_page* page = pi->page;
            if (pdf_to_num(ctx, page->obj) == 0)
                return false;
        }
    }
    return true;
}

void PdfEngineImpl::UpdateUserAnnotations(Vec<PageAnnotation>* list) {
    // TODO: use a new critical section to avoid blocking the UI thread
    ScopedCritSec scope(&ctxAccess);
    if (list)
        userAnnots = *list;
    else
        userAnnots.Reset();
}

char* PdfEngineImpl::GetDecryptionKey() const {
    if (!_decryptionKey)
        return nullptr;
    return str::Dup(_decryptionKey);
}

PageLayoutType PdfEngineImpl::PreferredLayout() {
    PageLayoutType layout = Layout_Single;

    ScopedCritSec scope(&ctxAccess);
    pdf_obj* root = nullptr;
    fz_try(ctx) {
        root = pdf_dict_gets(ctx, pdf_trailer(ctx, _doc), "Root");
    }
    fz_catch(ctx) {
        return layout;
    }

    fz_try(ctx) {
        const char* name = pdf_to_name(ctx, pdf_dict_gets(ctx, root, "PageLayout"));
        if (str::EndsWith(name, "Right"))
            layout = Layout_Book;
        else if (str::StartsWith(name, "Two"))
            layout = Layout_Facing;
    }
    fz_catch(ctx) {
    }

    fz_try(ctx) {
        pdf_obj* prefs = pdf_dict_gets(ctx, root, "ViewerPreferences");
        const char* direction = pdf_to_name(ctx, pdf_dict_gets(ctx, prefs, "Direction"));
        if (str::Eq(direction, "R2L"))
            layout = (PageLayoutType)(layout | Layout_R2L);
    }
    fz_catch(ctx) {
    }

    return layout;
}

u8* PdfEngineImpl::GetFileData(size_t* cbCount) {
    u8* res = nullptr;
    ScopedCritSec scope(&ctxAccess);
    fz_try(ctx) {
        res = fz_extract_stream_data(ctx, _doc->file, cbCount);
    }
    fz_catch(ctx) {
        res = nullptr;
        if (FileName()) {
            OwnedData data(file::ReadFile(FileName()));
            if (cbCount) {
                *cbCount = data.size;
            }
            res = (u8*)data.StealData();
        }
    }
    return res;
}

bool PdfEngineImpl::SaveFileAs(const char* copyFileName, bool includeUserAnnots) {
    size_t dataLen;
    AutoFreeW dstPath(str::conv::FromUtf8(copyFileName));
    ScopedMem<unsigned char> data(GetFileData(&dataLen));
    if (data) {
        bool ok = file::WriteFile(dstPath, data.Get(), dataLen);
        if (ok) {
            return !includeUserAnnots || SaveUserAnnots(copyFileName);
        }
    }
    if (!FileName()) {
        return false;
    }
    bool ok = CopyFileW(FileName(), dstPath, FALSE);
    if (!ok) {
        return false;
    }
    // TODO: try to recover when SaveUserAnnots fails?
    return !includeUserAnnots || SaveUserAnnots(copyFileName);
}

static bool pdf_file_update_add_annotation(fz_context* ctx, pdf_document* doc, pdf_page* page, pdf_obj* page_obj,
                                           PageAnnotation& annot, pdf_obj* annots) {
    CrashMePort();
#if 0
    static const char* obj_dict =
        "<<\
    /Type /Annot /Subtype /%s\
    /Rect [%f %f %f %f]\
    /C [%f %f %f]\
    /F %d\
    /P %d %d R\
    /QuadPoints %s\
    /AP << >>\
>>";
    static const char* obj_quad_tpl = "[%f %f %f %f %f %f %f %f]";
    static const char* ap_dict =
        "<< /Type /XObject /Subtype /Form /BBox [0 0 %f %f] /Resources << /ExtGState << /GS << /Type /ExtGState "
        "/ca "
        "%.f /AIS false /BM /Multiply >> >> /ProcSet [/PDF] >> >>";
    static const char* ap_highlight = "q /DeviceRGB cs /GS gs %f %f %f rg 0 0 %f %f re f Q\n";
    static const char* ap_underline = "q /DeviceRGB CS %f %f %f RG 1 w [] 0 d 0 0.5 m %f 0.5 l S Q\n";
    static const char* ap_strikeout = "q /DeviceRGB CS %f %f %f RG 1 w [] 0 d 0 %f m %f %f l S Q\n";
    static const char* ap_squiggly =
        "q /DeviceRGB CS %f %f %f RG 0.5 w [1] 1.5 d 0 0.25 m %f 0.25 l S [1] 0.5 d 0 0.75 m %f 0.75 l S Q\n";

    pdf_obj *annot_obj = nullptr, *ap_obj = nullptr;
    fz_buffer* ap_buf = nullptr;

    fz_var(annot_obj);
    fz_var(ap_obj);
    fz_var(ap_buf);

    const char* subtype = PageAnnotType::Highlight == annot.type
                              ? "Highlight"
                              : PageAnnotType::Underline == annot.type
                                    ? "Underline"
                                    : PageAnnotType::StrikeOut == annot.type
                                          ? "StrikeOut"
                                          : PageAnnotType::Squiggly == annot.type ? "Squiggly" : nullptr;
    CrashIf(!subtype);
    int rotation = (page->rotate + 360) % 360;
    CrashIf((rotation % 90) != 0);
    // convert the annotation's rectangle back to raw user space
    fz_rect r = fz_RectD_to_rect(annot.rect);
    fz_matrix invctm = fz_invert_matrix(page->ctm)
    fz_transform_rect(&r, invctm);
    double dx = r.x1 - r.x0, dy = r.y1 - r.y0;
    if ((rotation % 180) == 90)
        std::swap(dx, dy);
    float rgb[3] = {annot.color.r / 255.f, annot.color.g / 255.f, annot.color.b / 255.f};
    // rotate the QuadPoints to match the page
    AutoFree quad_tpl;
    if (0 == rotation)
        quad_tpl.Set(str::Format(obj_quad_tpl, r.x0, r.y1, r.x1, r.y1, r.x0, r.y0, r.x1, r.y0));
    else if (90 == rotation)
        quad_tpl.Set(str::Format(obj_quad_tpl, r.x0, r.y0, r.x0, r.y1, r.x1, r.y0, r.x1, r.y1));
    else if (180 == rotation)
        quad_tpl.Set(str::Format(obj_quad_tpl, r.x1, r.y0, r.x0, r.y0, r.x1, r.y1, r.x0, r.y1));
    else // if (270 == rotation)
        quad_tpl.Set(str::Format(obj_quad_tpl, r.x1, r.y1, r.x1, r.y0, r.x0, r.y1, r.x0, r.y0));
    AutoFree annot_tpl(str::Format(obj_dict, subtype, r.x0, r.y0, r.x1, r.y1, rgb[0], rgb[1],
                                   rgb[2],                                              // Rect and Color
                                   F_Print, pdf_to_num(ctx, page_obj), pdf_to_gen(ctx, page_obj), // F and P
                                   quad_tpl.Get()));
    AutoFree annot_ap_dict(str::Format(ap_dict, dx, dy, annot.color.a / 255.f));
    AutoFree annot_ap_stream;

    fz_try(ctx) {
        annot_obj = pdf_new_obj_from_str(ctx, doc, annot_tpl);
        // append the annotation to the file
        pdf_array_push_drop(ctx, annots, pdf_new_ref(doc, annot_obj));
    }
    fz_catch(ctx) {
        pdf_drop_obj(ctx, annot_obj);
        return false;
    }

    if (doc->crypt) {
        // since we don't encrypt the appearance stream, for encrypted documents
        // the readers will have to synthesize an appearance stream themselves
        pdf_drop_obj(ctx, annot_obj);
        return true;
    }

    fz_try(ctx) {
        // create the appearance stream (unencrypted) and append it to the file
        ap_obj = pdf_new_obj_from_str(ctx, doc, annot_ap_dict);
        switch (annot.type) {
            case PageAnnotType::Highlight:
                annot_ap_stream.Set(str::Format(ap_highlight, rgb[0], rgb[1], rgb[2], dx, dy));
                break;
            case PageAnnotType::Underline:
                annot_ap_stream.Set(str::Format(ap_underline, rgb[0], rgb[1], rgb[2], dx));
                break;
            case PageAnnotType::StrikeOut:
                annot_ap_stream.Set(str::Format(ap_strikeout, rgb[0], rgb[1], rgb[2], dy / 2, dx, dy / 2));
                break;
            case PageAnnotType::Squiggly:
                annot_ap_stream.Set(str::Format(ap_squiggly, rgb[0], rgb[1], rgb[2], dx, dx));
                break;
        }
        if (annot.type != PageAnnotType::Highlight)
            pdf_dict_dels(ctx, pdf_dict_gets(ctx, ap_obj, "Resources"), "ExtGState");
        if (rotation) {
            pdf_dict_puts_drop(ctx, ap_obj, "Matrix", pdf_new_matrix(ctx, doc, fz_rotate(rotation)));
        }
        ap_buf = fz_new_buffer(ctx, (int)str::Len(annot_ap_stream));
        memcpy(ap_buf->data, annot_ap_stream, (ap_buf->len = (int)str::Len(annot_ap_stream)));
        pdf_dict_puts_drop(ctx, ap_obj, "Length", pdf_new_int(ctx, doc, ap_buf->len));
        // append the appearance stream to the file
        int num = pdf_create_object(ctx, doc);
        pdf_update_object(ctx, doc, num, ap_obj);
        pdf_update_stream(ctx, doc, num, ap_buf);
        pdf_dict_puts_drop(ctx, pdf_dict_gets(ctx, annot_obj, "AP"), "N", pdf_new_indirect(ctx, doc, num, 0));
    }
    fz_always(ctx) {
        pdf_drop_obj(ctx, ap_obj);
        fz_drop_buffer(ctx, ap_buf);
        pdf_drop_obj(ctx, annot_obj);
    }
    fz_catch(ctx) { return false; }
#endif
    return true;
}

bool PdfEngineImpl::SaveUserAnnots(const char* pathUtf8) {
    if (!userAnnots.size())
        return true;

    ScopedCritSec scope1(&pagesAccess);
    ScopedCritSec scope2(&ctxAccess);

    bool ok = true;
    Vec<PageAnnotation> pageAnnots;

    fz_try(ctx) {
        for (int pageNo = 1; pageNo <= PageCount(); pageNo++) {
            pdf_page* page = GetPdfPage(pageNo);
            pdf_obj* page_obj = page->obj;
            // TODO: this will skip annotations for broken documents
            if (!page || !pdf_to_num(ctx, page_obj)) {
                ok = false;
                break;
            }
            pageAnnots = fz_get_user_page_annots(userAnnots, pageNo);
            if (pageAnnots.size() == 0)
                continue;
            // get the page's /Annots array for appending
            pdf_obj* annots = pdf_dict_gets(ctx, page_obj, "Annots");
            if (!pdf_is_array(ctx, annots)) {
                pdf_dict_puts_drop(ctx, page_obj, "Annots", pdf_new_array(ctx, _doc, (int)pageAnnots.size()));
                annots = pdf_dict_gets(ctx, page_obj, "Annots");
            }
            if (!pdf_is_indirect(ctx, annots)) {
                // make /Annots indirect for the current /Page
                CrashMePort();
                // TODO(port): no pdf_new_ref
                // pdf_dict_puts_drop(ctx, page_obj, "Annots", pdf_new_ref(ctx, _doc, annots));
            }
            // append all annotations for the current page
            for (size_t i = 0; i < pageAnnots.size(); i++) {
                ok &= pdf_file_update_add_annotation(ctx, _doc, page, page_obj, pageAnnots.at(i), annots);
            }
        }
        if (ok) {
            pdf_write_options opts = {0};
            opts.do_incremental = 1;
            pdf_save_document(ctx, _doc, const_cast<char*>(pathUtf8), &opts);
        }
    }
    fz_catch(ctx) {
        ok = false;
    }
    return ok;
}

bool PdfEngineImpl::SaveEmbedded(LinkSaverUI& saveUI, int num) {
    ScopedCritSec scope(&ctxAccess);

    fz_buffer* buf = nullptr;
    fz_try(ctx) {
        buf = pdf_load_stream_number(ctx, _doc, num);
    }
    fz_catch(ctx) {
        return false;
    }
    CrashIf(nullptr == buf);
    u8* data = nullptr;
    size_t dataLen = fz_buffer_extract(ctx, buf, &data);
    bool result = saveUI.SaveEmbedded(data, dataLen);
    fz_drop_buffer(ctx, buf);
    return result;
}

bool PdfEngineImpl::HasClipOptimizations(int pageNo) {
    PdfPageInfo* pi = GetPdfPageInfo(pageNo, true);
    if (!pi) {
        return false;
    }

    // GetPdfPageInfo extracts imageRects for us
    if (!pi->imageRects) {
        return true;
    }

    fz_rect mbox = fz_RectD_to_rect(PageMediabox(pageNo));
    // check if any image covers at least 90% of the page
    for (int i = 0; !fz_is_empty_rect(pi->imageRects[i]); i++) {
        if (fz_calc_overlap(mbox, pi->imageRects[i]) >= 0.9f) {
            return false;
        }
    }
    return true;
}

WCHAR* PdfEngineImpl::GetPageLabel(int pageNo) const {
    if (!_pagelabels || pageNo < 1 || PageCount() < pageNo) {
        return BaseEngine::GetPageLabel(pageNo);
    }

    return str::Dup(_pagelabels->at(pageNo - 1));
}

int PdfEngineImpl::GetPageByLabel(const WCHAR* label) const {
    int pageNo = 0;
    if (_pagelabels) {
        pageNo = _pagelabels->Find(label) + 1;
    }

    if (!pageNo) {
        return BaseEngine::GetPageByLabel(label);
    }

    return pageNo;
}

// copy of fz_is_external_link without ctx
static int is_external_link(const char* uri) {
    while (*uri >= 'a' && *uri <= 'z')
        ++uri;
    return uri[0] == ':';
}

// copy of pdf_resolve_link in pdf-link.c without ctx and doc
// returns page number and location on the page
int resolve_link(const char* uri, float* xp, float* yp) {
    if (uri && uri[0] == '#') {
        int page = fz_atoi(uri + 1) - 1;
        if (xp || yp) {
            const char* x = strchr(uri, ',');
            const char* y = strrchr(uri, ',');
            if (x && y) {
                if (xp)
                    *xp = fz_atoi(x + 1);
                if (yp)
                    *yp = fz_atoi(y + 1);
            }
        }
        return page;
    }
    return -1;
}

#if 0
static bool IsRelativeURI(const WCHAR* uri) {
    const WCHAR* c = uri;
    while (*c && *c != ':' && *c != '/' && *c != '?' && *c != '#') {
        c++;
    }
    return *c != ':';
}
#endif

PdfLink::PdfLink(PdfEngineImpl* engine, int pageNo, fz_link* link, fz_outline* outline) {
    this->engine = engine;
    this->pageNo = pageNo;
    CrashIf(!link && !outline);
    this->link = link;
    this->outline = outline;
}

RectD PdfLink::GetRect() const {
    if (link) {
        RectD r(fz_rect_to_RectD(link->rect));
        return r;
    }
    CrashMePort();
    return RectD();
}

static char* PdfLinkGetURI(const PdfLink* link) {
    if (link->link) {
        return link->link->uri;
    }
    if (link->outline) {
        return link->outline->uri;
    }
    CrashMePort();
    return nullptr;
}

WCHAR* PdfLink::GetValue() const {
    if (outline && isAttachment) {
        WCHAR* path = str::conv::FromUtf8(outline->uri);
        return path;
    }

    char* uri = PdfLinkGetURI(this);
    if (!is_external_link(uri)) {
        // other values: #1,115,208
        OutputDebugStringA("unknown link:");
        OutputDebugStringA(uri);
        OutputDebugStringA("\n");
        // CrashMePort();
        return nullptr;
    }
    OutputDebugStringA("PdfLink:");
    OutputDebugStringA(uri);
    OutputDebugStringA("\n");
    // CrashMePort();
    WCHAR* path = str::conv::FromUtf8(uri);
    return path;
#if 0
    if (!link || !engine)
        return nullptr;
    if (link->kind != FZ_LINK_URI && link->kind != FZ_LINK_LAUNCH && link->kind != FZ_LINK_GOTOR)
        return nullptr;

    ScopedCritSec scope(&engine->ctxAccess);

    WCHAR* path = nullptr;

    switch (link->kind) {
        case FZ_LINK_URI:
            path = str::conv::FromUtf8(link->ld.uri.uri);
            if (IsRelativeURI(path)) {
                AutoFreeW base;
                fz_try(engine->ctx) {
                    pdf_obj* obj = pdf_dict_gets(pdf_trailer(engine->_doc), "Root");
                    obj = pdf_dict_gets(pdf_dict_gets(obj, "URI"), "Base");
                    if (obj)
                        base.Set(str::conv::FromPdf(obj));
                }
                fz_catch(engine->ctx) {}
                if (!str::IsEmpty(base.Get())) {
                    AutoFreeW uri(str::Join(base, path));
                    free(path);
                    path = uri.StealData();
                }
            }
            if (link->ld.uri.is_map) {
                int x = 0, y = 0;
                if (rect.Contains(pt)) {
                    x = (int)(pt.x - rect.x + 0.5);
                    y = (int)(pt.y - rect.y + 0.5);
                }
                AutoFreeW uri(str::Format(L"%s?%d,%d", path, x, y));
                free(path);
                path = uri.StealData();
            }
            break;
        case FZ_LINK_LAUNCH:
            // note: we (intentionally) don't support the /Win specific Launch parameters
            if (link->ld.launch.file_spec)
                path = str::conv::FromUtf8(link->ld.launch.file_spec);
            if (path && link->ld.launch.embedded_num && str::EndsWithI(path, L".pdf")) {
                free(path);
                path = str::Format(L"%s:%d:%d", engine->FileName(), link->ld.launch.embedded_num,
                                   link->ld.launch.embedded_gen);
            }
            break;
        case FZ_LINK_GOTOR:
            if (link->ld.gotor.file_spec)
                path = str::conv::FromUtf8(link->ld.gotor.file_spec);
            break;
    }

    return path;
#endif
}

#if 0
static PageDestType DestTypeFromName(const char* name) {
// named actions are converted either to Dest_Name or Dest_NameDialog
#define HandleType(type)      \
    if (str::Eq(name, #type)) \
    return PageDestType::##type
#define HandleTypeDialog(type) \
    if (str::Eq(name, #type))  \
    return PageDestType::##type##Dialog
    // predefined named actions
    HandleType(NextPage);
    HandleType(PrevPage);
    HandleType(FirstPage);
    HandleType(LastPage);
    // Adobe Reader extensions to the spec
    // cf. http://www.tug.org/applications/hyperref/manual.html
    HandleTypeDialog(Find);
    HandleType(FullScreen);
    HandleType(GoBack);
    HandleType(GoForward);
    HandleTypeDialog(GoToPage);
    HandleTypeDialog(Print);
    HandleTypeDialog(SaveAs);
    HandleTypeDialog(ZoomTo);
#undef HandleType
#undef HandleTypeDialog
    // named action that we don't support (or invalid action name)
    return PageDestType::None;
}
#endif

PageDestType PdfLink::GetDestType() const {
    if (outline && isAttachment) {
        return PageDestType::LaunchEmbedded;
    }

    char* uri = PdfLinkGetURI(this);
    CrashIf(!uri);
    if (!uri) {
        return PageDestType::None;
    }
    if (!is_external_link(uri)) {
        float x, y;
        int pageNo = resolve_link(uri, &x, &y);
        if (pageNo == -1) {
            // TODO: figure out what it could be
            CrashMePort();
            return PageDestType::None;
        }
        return PageDestType::ScrollTo;
    }
    if (str::StartsWith(uri, "file://")) {
        return PageDestType::LaunchFile;
    }
    if (str::StartsWithI(uri, "http://")) {
        return PageDestType::LaunchURL;
    }
    if (str::StartsWithI(uri, "https://")) {
        return PageDestType::LaunchURL;
    }
    if (str::StartsWithI(uri, "ftp://")) {
        return PageDestType::LaunchURL;
    }
    // TODO: PageDestType::LaunchEmbedded, PageDestType::LaunchURL, named destination

    CrashMePort();
    return PageDestType::None;
#if 0
    switch (link->kind) {
        case FZ_LINK_GOTO:
            return PageDestType::ScrollTo;
        case FZ_LINK_URI:
            return PageDestType::LaunchURL;
        case FZ_LINK_NAMED:
            return DestTypeFromName(link->ld.named.named);
        case FZ_LINK_LAUNCH:
            if (link->ld.launch.embedded_num)
                return PageDestType::LaunchEmbedded;
            if (link->ld.launch.is_uri)
                return PageDestType::LaunchURL;
            return PageDestType::LaunchFile;
        case FZ_LINK_GOTOR:
            return PageDestType::LaunchFile;
        default:
            return PageDestType::None; // unsupported action
    }
#endif
}

int PdfLink::GetDestPageNo() const {
    char* uri = PdfLinkGetURI(this);
    CrashIf(!uri);
    if (!uri) {
        return 0;
    }
    if (is_external_link(uri)) {
        return 0;
    }
    float x, y;
    int pageNo = resolve_link(uri, &x, &y);
    if (pageNo == -1) {
        return 0;
    }
    return pageNo + 1; // TODO(port): or is it just pageNo?
#if 0
    if (link && FZ_LINK_GOTO == link->kind)
        return link->ld.gotor.page + 1;
    if (link && FZ_LINK_GOTOR == link->kind && !link->ld.gotor.dest)
        return link->ld.gotor.page + 1;
#endif
    return 0;
}

RectD PdfLink::GetDestRect() const {
    RectD result(DEST_USE_DEFAULT, DEST_USE_DEFAULT, DEST_USE_DEFAULT, DEST_USE_DEFAULT);
    char* uri = PdfLinkGetURI(this);
    CrashIf(!uri);
    if (!uri) {
        CrashMePort();
        return result;
    }

    if (is_external_link(uri)) {
        return result;
    }
    float x, y;
    int pageNo = resolve_link(uri, &x, &y);
    if (pageNo == -1) {
        CrashMePort();
        return result;
    }

    // TODO(port): should those be trasformed by page's ctm?
    result.x = (double)x;
    result.y = (double)y;
    return result;
#if 0
    if (!link || FZ_LINK_GOTO != link->kind && FZ_LINK_GOTOR != link->kind)
        return result;
    if (link->ld.gotor.page < 0 || link->ld.gotor.page >= engine->PageCount())
        return result;

    pdf_page* page = engine->GetPdfPage(link->ld.gotor.page + 1);
    if (!page)
        return result;
    fz_point lt = link->ld.gotor.lt, rb = link->ld.gotor.rb;
    fz_transform_point(&lt, &page->ctm);
    fz_transform_point(&rb, &page->ctm);

    if ((link->ld.gotor.flags & fz_link_flag_r_is_zoom)) {
        // /XYZ link, undefined values for the coordinates mean: keep the current position
        if ((link->ld.gotor.flags & fz_link_flag_l_valid))
            result.x = lt.x;
        if ((link->ld.gotor.flags & fz_link_flag_t_valid))
            result.y = lt.y;
        result.dx = result.dy = 0;
    } else if ((link->ld.gotor.flags & (fz_link_flag_fit_h | fz_link_flag_fit_v)) ==
                   (fz_link_flag_fit_h | fz_link_flag_fit_v) &&
               (link->ld.gotor.flags &
                (fz_link_flag_l_valid | fz_link_flag_t_valid | fz_link_flag_r_valid | fz_link_flag_b_valid))) {
        // /FitR link
        result = RectD::FromXY(lt.x, lt.y, rb.x, rb.y);
        // an empty destination rectangle would imply an /XYZ-type link to callers
        if (result.IsEmpty())
            result.dx = result.dy = 0.1;
    } else if ((link->ld.gotor.flags & (fz_link_flag_fit_h | fz_link_flag_fit_v)) == fz_link_flag_fit_h &&
               (link->ld.gotor.flags & fz_link_flag_t_valid)) {
        // /FitH or /FitBH link
        result.y = lt.y;
    }
    // all other link types only affect the zoom level, which we intentionally leave alone
#endif
}

WCHAR* PdfLink::GetDestName() const {
    char* uri = PdfLinkGetURI(this);
    if (is_external_link(uri)) {
        return nullptr;
    }
    // TODO(port): test with more stuff
    // figure out what PDF_NAME(GoToR) ends up being
    return nullptr;
#if 0
    if (!link || FZ_LINK_GOTOR != link->kind || !link->ld.gotor.dest)
        return nullptr;
    return str::conv::FromUtf8(link->ld.gotor.dest);
#endif
}

bool PdfLink::SaveEmbedded(LinkSaverUI& saveUI) {
    CrashIf(!outline || !isAttachment);

    ScopedCritSec scope(&engine->ctxAccess);
    // TODO: hack, we stored stream number in outline->page
    return engine->SaveEmbedded(saveUI, outline->page);
}

BaseEngine* PdfEngineImpl::CreateFromFile(const WCHAR* fileName, PasswordUI* pwdUI) {
    PdfEngineImpl* engine = new PdfEngineImpl();
    if (!engine || !fileName || !engine->Load(fileName, pwdUI)) {
        delete engine;
        return nullptr;
    }
    return engine;
}

BaseEngine* PdfEngineImpl::CreateFromStream(IStream* stream, PasswordUI* pwdUI) {
    PdfEngineImpl* engine = new PdfEngineImpl();
    if (!engine->Load(stream, pwdUI)) {
        delete engine;
        return nullptr;
    }
    return engine;
}

namespace PdfEngine {

bool IsSupportedFile(const WCHAR* fileName, bool sniff) {
    if (sniff) {
        char header[1024] = {0};
        file::ReadN(fileName, header, sizeof(header));

        for (int i = 0; i < sizeof(header) - 4; i++) {
            if (str::EqN(header + i, "%PDF", 4))
                return true;
        }
        return false;
    }

    return str::EndsWithI(fileName, L".pdf") || findEmbedMarks(fileName);
}

BaseEngine* CreateFromFile(const WCHAR* fileName, PasswordUI* pwdUI) {
    return PdfEngineImpl::CreateFromFile(fileName, pwdUI);
}

BaseEngine* CreateFromStream(IStream* stream, PasswordUI* pwdUI) {
    return PdfEngineImpl::CreateFromStream(stream, pwdUI);
}

} // namespace PdfEngine

// TODO: nasty but I want them in separate files
#include "XpsEngine.cpp"
