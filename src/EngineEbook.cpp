/* Copyright 2019 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

// engines which render flowed ebook formats into fixed pages through the EngineBase API
// (pages are mostly layed out the same as for a "B Format" paperback: 5.12" x 7.8")

#include "utils/BaseUtil.h"
#include "utils/ScopedWin.h"
#include "utils/Archive.h"
#include "utils/Dpi.h"
#include "utils/FileUtil.h"
#include "utils/GdiPlusUtil.h"
#include "utils/HtmlParserLookup.h"
#include "utils/HtmlPullParser.h"
#include "mui/Mui.h"
#include "utils/PalmDbReader.h"
#include "utils/TrivialHtmlParser.h"
#include "utils/WinUtil.h"
#include "utils/ZipUtil.h"

#include "wingui/TreeModel.h"
#include "EngineBase.h"
#include "EngineEbook.h"
#include "EbookBase.h"
#include "EbookDoc.h"
#include "HtmlFormatter.h"
#include "EbookFormatter.h"

Kind kindEngineEpub = "engineEpub";
Kind kindEngineFb2 = "engineFb2";
Kind kindEngineMobi = "engineMobi";
Kind kindEnginePdb = "enginePdb";
Kind kindEngineChm = "engineChm";
Kind kindEngineHtml = "engineHtml";
Kind kindEngineTxt = "engineTxt";

static AutoFreeWstr gDefaultFontName;
static float gDefaultFontSize = 10.f;

static const WCHAR* GetDefaultFontName() {
    return gDefaultFontName.get() ? gDefaultFontName.get() : L"Georgia";
}

static float GetDefaultFontSize() {
    // fonts are scaled at higher DPI settings,
    // undo this here for (mostly) consistent results
    return gDefaultFontSize * 96.0f / DpiGetPreciseY(HWND_DESKTOP);
}

void SetDefaultEbookFont(const WCHAR* name, float size) {
    // intentionally don't validate the input
    gDefaultFontName.SetCopy(name);
    // use a somewhat smaller size than in the EbookUI, since fit page/width
    // is likely to be above 100% for the paperback page dimensions
    gDefaultFontSize = size * 0.8f;
}

/* common classes for EPUB, FictionBook2, Mobi, PalmDOC, CHM, HTML and TXT engines */

struct PageAnchor {
    DrawInstr* instr;
    int pageNo;

    explicit PageAnchor(DrawInstr* instr = nullptr, int pageNo = -1) : instr(instr), pageNo(pageNo) {
    }
};

class EbookAbortCookie : public AbortCookie {
  public:
    bool abort;
    EbookAbortCookie() : abort(false) {
    }
    void Abort() override {
        abort = true;
    }
};

class EbookEngine : public EngineBase {
  public:
    EbookEngine();
    virtual ~EbookEngine();

    int PageCount() const override {
        return pages ? (int)pages->size() : 0;
    }

    RectD PageMediabox(int pageNo) override {
        UNUSED(pageNo);
        return pageRect;
    }
    RectD PageContentBox(int pageNo, RenderTarget target = RenderTarget::View) override {
        UNUSED(target);
        RectD mbox = PageMediabox(pageNo);
        mbox.Inflate(-pageBorder, -pageBorder);
        return mbox;
    }

    RenderedBitmap* RenderBitmap(int pageNo, float zoom, int rotation,
                                 RectD* pageRect = nullptr, /* if nullptr: defaults to the page's mediabox */
                                 RenderTarget target = RenderTarget::View, AbortCookie** cookie_out = nullptr) override;

    PointD Transform(PointD pt, int pageNo, float zoom, int rotation, bool inverse = false) override;
    RectD Transform(RectD rect, int pageNo, float zoom, int rotation, bool inverse = false) override;

    std::string_view GetFileData() override {
        if (!fileName) {
            return {};
        }
        return file::ReadFile(fileName.get());
    }

    bool SaveFileAs(const char* copyFileName, bool includeUserAnnots = false) override {
        UNUSED(includeUserAnnots);
        if (!fileName) {
            return false;
        }
        AutoFreeWstr path(strconv::FromUtf8(copyFileName));
        return fileName ? CopyFileW(fileName, path, FALSE) : false;
    }
    WCHAR* ExtractPageText(int pageNo, RectI** coordsOut = nullptr) override;
    // make RenderCache request larger tiles than per default
    bool HasClipOptimizations(int pageNo) override {
        UNUSED(pageNo);
        return false;
    }

    bool SupportsAnnotation(bool forSaving = false) const override {
        return !forSaving;
    }
    void UpdateUserAnnotations(Vec<PageAnnotation>* list) override;

    Vec<PageElement*>* GetElements(int pageNo) override;
    PageElement* GetElementAtPos(int pageNo, PointD pt) override;

    PageDestination* GetNamedDest(const WCHAR* name) override;
    RenderedBitmap* GetImageForPageElement(PageElement* el) override;

    bool BenchLoadPage(int pageNo) override {
        UNUSED(pageNo);
        return true;
    }

  protected:
    Vec<HtmlPage*>* pages = nullptr;
    Vec<PageAnchor> anchors;
    // contains for each page the last anchor indicating
    // a break between two merged documents
    Vec<DrawInstr*> baseAnchors;
    // needed so that memory allocated by ResolveHtmlEntities isn't leaked
    PoolAllocator allocator;
    // TODO: still needed?
    CRITICAL_SECTION pagesAccess;
    // access to userAnnots is protected by pagesAccess
    Vec<PageAnnotation> userAnnots;
    // page dimensions can vary between filetypes
    RectD pageRect;
    float pageBorder;

    void GetTransform(Matrix& m, float zoom, int rotation) {
        GetBaseTransform(m, pageRect.ToGdipRectF(), zoom, rotation);
    }
    bool ExtractPageAnchors();
    WCHAR* ExtractFontList();

    virtual PageElement* CreatePageLink(DrawInstr* link, RectI rect, int pageNo);

    Vec<DrawInstr>* GetHtmlPage(int pageNo) {
        CrashIf(pageNo < 1 || PageCount() < pageNo);
        if (pageNo < 1 || PageCount() < pageNo)
            return nullptr;
        return &pages->at(pageNo - 1)->instructions;
    }
};

static PageElement* newEbookLink(DrawInstr* link, RectI rect, PageDestination* dest, int pageNo = 0,
                                 bool showUrl = false) {
    auto res = new PageElement();
    res->pageNo = pageNo;

    res->kind = kindPageElementDest;
    res->rect = rect.Convert<double>();

    if (!dest || showUrl) {
        res->value = strconv::FromHtmlUtf8(link->str.s, link->str.len);
    }

    if (!dest) {
        dest = new PageDestination();
        dest->kind = kindDestinationLaunchURL;
        // TODO: not sure about this
        dest->value = str::Dup(res->value);
        dest->pageNo = 0;
        dest->rect = rect.Convert<double>();
    }
    res->dest = dest;
    return res;
}

static RenderedBitmap* getImageFromData(ImageData* id) {
    HBITMAP hbmp;
    Bitmap* bmp = BitmapFromData(id->data, id->len);
    if (!bmp || bmp->GetHBITMAP((ARGB)Color::White, &hbmp) != Ok) {
        delete bmp;
        return nullptr;
    }
    SizeI size(bmp->GetWidth(), bmp->GetHeight());
    delete bmp;
    return new RenderedBitmap(hbmp, size);
}

static PageElement* newImageDataElement(int pageNo, ImageData* id, RectI bbox) {
    auto res = new PageElement();
    res->kind = kindPageElementImage;
    res->pageNo = pageNo;
    res->rect = bbox.Convert<double>();
    res->getImage = [=]() -> RenderedBitmap* { return getImageFromData(id); };
    return res;
}

class EbookTocItem : public DocTocItem {
  public:
    EbookTocItem(WCHAR* title, PageDestination* dest) : DocTocItem(title, 0) {
        this->dest = dest;
        if (dest) {
            pageNo = dest->GetPageNo();
        }
    }
};

EbookEngine::EbookEngine() {
    // "B Format" paperback
    pageRect = RectD(0, 0, 5.12 * GetFileDPI(), 7.8 * GetFileDPI());
    pageBorder = 0.4f * GetFileDPI();
    preferredLayout = Layout_Book;
    InitializeCriticalSection(&pagesAccess);
}

EbookEngine::~EbookEngine() {
    EnterCriticalSection(&pagesAccess);

    if (pages) {
        DeleteVecMembers(*pages);
    }
    delete pages;

    LeaveCriticalSection(&pagesAccess);
    DeleteCriticalSection(&pagesAccess);
}

bool EbookEngine::ExtractPageAnchors() {
    ScopedCritSec scope(&pagesAccess);

    DrawInstr* baseAnchor = nullptr;
    for (int pageNo = 1; pageNo <= PageCount(); pageNo++) {
        Vec<DrawInstr>* pageInstrs = GetHtmlPage(pageNo);
        if (!pageInstrs)
            return false;

        for (size_t k = 0; k < pageInstrs->size(); k++) {
            DrawInstr* i = &pageInstrs->at(k);
            if (DrawInstrType::Anchor != i->type)
                continue;
            anchors.Append(PageAnchor(i, pageNo));
            if (k < 2 && str::StartsWith(i->str.s + i->str.len, "\" page_marker />"))
                baseAnchor = i;
        }
        baseAnchors.Append(baseAnchor);
    }

    CrashIf(baseAnchors.size() != pages->size());
    return true;
}

PointD EbookEngine::Transform(PointD pt, int pageNo, float zoom, int rotation, bool inverse) {
    RectD rect = Transform(RectD(pt, SizeD()), pageNo, zoom, rotation, inverse);
    return PointD(rect.x, rect.y);
}

RectD EbookEngine::Transform(RectD rect, int pageNo, float zoom, int rotation, bool inverse) {
    UNUSED(pageNo);
    geomutil::RectT<REAL> rcF = rect.Convert<REAL>();
    PointF pts[2] = {PointF(rcF.x, rcF.y), PointF(rcF.x + rcF.dx, rcF.y + rcF.dy)};
    Matrix m;
    GetTransform(m, zoom, rotation);
    if (inverse)
        m.Invert();
    m.TransformPoints(pts, 2);
    return RectD::FromXY(pts[0].X, pts[0].Y, pts[1].X, pts[1].Y);
}

static void DrawAnnotations(Graphics& g, Vec<PageAnnotation>& userAnnots, int pageNo) {
    for (size_t i = 0; i < userAnnots.size(); i++) {
        PageAnnotation& annot = userAnnots.at(i);
        if (annot.pageNo != pageNo)
            continue;
        PointF p1, p2;
        switch (annot.type) {
            case PageAnnotType::Highlight: {
                SolidBrush tmpBrush(Unblend(annot.color, 119));
                g.FillRectangle(&tmpBrush, annot.rect.ToGdipRectF());
            } break;
            case PageAnnotType::Underline:
                p1 = PointF((float)annot.rect.x, (float)annot.rect.BR().y);
                p2 = PointF((float)annot.rect.BR().x, p1.Y);
                {
                    Pen tmpPen(FromColor(annot.color));
                    g.DrawLine(&tmpPen, p1, p2);
                }
                break;
            case PageAnnotType::StrikeOut:
                p1 = PointF((float)annot.rect.x, (float)annot.rect.y + (float)annot.rect.dy / 2);
                p2 = PointF((float)annot.rect.BR().x, p1.Y);
                {
                    Pen tmpPen(FromColor(annot.color));
                    g.DrawLine(&tmpPen, p1, p2);
                }
                break;
            case PageAnnotType::Squiggly: {
                Pen p(FromColor(annot.color), 0.5f);
                REAL dash[2] = {2, 2};
                p.SetDashPattern(dash, dimof(dash));
                p.SetDashOffset(1);
                p1 = PointF((float)annot.rect.x, (float)annot.rect.BR().y - 0.25f);
                p2 = PointF((float)annot.rect.BR().x, p1.Y);
                g.DrawLine(&p, p1, p2);
                p.SetDashOffset(3);
                p1.Y += 0.5f;
                p2.Y += 0.5f;
                g.DrawLine(&p, p1, p2);
            } break;
        }
    }
}

RenderedBitmap* EbookEngine::RenderBitmap(int pageNo, float zoom, int rotation, RectD* pageRect, RenderTarget target,
                                          AbortCookie** cookieOut) {
    UNUSED(target);
    RectD pageRc = pageRect ? *pageRect : PageMediabox(pageNo);
    RectI screen = Transform(pageRc, pageNo, zoom, rotation).Round();
    PointI screenTL = screen.TL();
    screen.Offset(-screen.x, -screen.y);

    HANDLE hMap = nullptr;
    HBITMAP hbmp = CreateMemoryBitmap(screen.Size(), &hMap);
    HDC hDC = CreateCompatibleDC(nullptr);
    DeleteObject(SelectObject(hDC, hbmp));

    Graphics g(hDC);
    mui::InitGraphicsMode(&g);

    Color white(0xFF, 0xFF, 0xFF);
    SolidBrush tmpBrush(white);
    Rect screenR(screen.ToGdipRect());
    screenR.Inflate(1, 1);
    g.FillRectangle(&tmpBrush, screenR);

    Matrix m;
    GetTransform(m, zoom, rotation);
    m.Translate((REAL)-screenTL.x, (REAL)-screenTL.y, MatrixOrderAppend);
    g.SetTransform(&m);

    EbookAbortCookie* cookie = nullptr;
    if (cookieOut)
        *cookieOut = cookie = new EbookAbortCookie();

    ScopedCritSec scope(&pagesAccess);

    mui::ITextRender* textDraw = mui::TextRenderGdiplus::Create(&g);
    DrawHtmlPage(&g, textDraw, GetHtmlPage(pageNo), pageBorder, pageBorder, false, Color((ARGB)Color::Black),
                 cookie ? &cookie->abort : nullptr);
    DrawAnnotations(g, userAnnots, pageNo);
    delete textDraw;
    DeleteDC(hDC);

    if (cookie && cookie->abort) {
        DeleteObject(hbmp);
        CloseHandle(hMap);
        return nullptr;
    }

    return new RenderedBitmap(hbmp, screen.Size(), hMap);
}

static RectI GetInstrBbox(DrawInstr& instr, float pageBorder) {
    geomutil::RectT<float> bbox(instr.bbox.X, instr.bbox.Y, instr.bbox.Width, instr.bbox.Height);
    bbox.Offset(pageBorder, pageBorder);
    return bbox.Round();
}

WCHAR* EbookEngine::ExtractPageText(int pageNo, RectI** coordsOut) {
    const WCHAR* lineSep = L"\n";
    ScopedCritSec scope(&pagesAccess);

    str::WStr content;
    content.allowFailure = true;
    Vec<RectI> coords;
    coords.allowFailure = true;
    bool insertSpace = false;

    Vec<DrawInstr>* pageInstrs = GetHtmlPage(pageNo);
    for (DrawInstr& i : *pageInstrs) {
        RectI bbox = GetInstrBbox(i, pageBorder);
        switch (i.type) {
            case DrawInstrType::String:
                if (coords.size() > 0 &&
                    (bbox.x < coords.Last().BR().x || bbox.y > coords.Last().y + coords.Last().dy * 0.8)) {
                    content.Append(lineSep);
                    coords.AppendBlanks(str::Len(lineSep));
                    CrashIf(*lineSep && !coords.Last().IsEmpty());
                } else if (insertSpace && coords.size() > 0) {
                    int swidth = bbox.x - coords.Last().BR().x;
                    if (swidth > 0) {
                        content.Append(' ');
                        coords.Append(RectI(bbox.x - swidth, bbox.y, swidth, bbox.dy));
                    }
                }
                insertSpace = false;
                {
                    AutoFreeWstr s(strconv::FromHtmlUtf8(i.str.s, i.str.len));
                    content.Append(s);
                    size_t len = str::Len(s);
                    double cwidth = 1.0 * bbox.dx / len;
                    for (size_t k = 0; k < len; k++)
                        coords.Append(RectI((int)(bbox.x + k * cwidth), bbox.y, (int)cwidth, bbox.dy));
                }
                break;
            case DrawInstrType::RtlString:
                if (coords.size() > 0 &&
                    (bbox.BR().x > coords.Last().x || bbox.y > coords.Last().y + coords.Last().dy * 0.8)) {
                    content.Append(lineSep);
                    coords.AppendBlanks(str::Len(lineSep));
                    CrashIf(*lineSep && !coords.Last().IsEmpty());
                } else if (insertSpace && coords.size() > 0) {
                    int swidth = coords.Last().x - bbox.BR().x;
                    if (swidth > 0) {
                        content.Append(' ');
                        coords.Append(RectI(bbox.BR().x, bbox.y, swidth, bbox.dy));
                    }
                }
                insertSpace = false;
                {
                    AutoFreeWstr s(strconv::FromHtmlUtf8(i.str.s, i.str.len));
                    content.Append(s);
                    size_t len = str::Len(s);
                    double cwidth = 1.0 * bbox.dx / len;
                    for (size_t k = 0; k < len; k++)
                        coords.Append(RectI((int)(bbox.x + (len - k - 1) * cwidth), bbox.y, (int)cwidth, bbox.dy));
                }
                break;
            case DrawInstrType::ElasticSpace:
            case DrawInstrType::FixedSpace:
                insertSpace = true;
                break;
        }
    }
    if (content.size() > 0 && !str::EndsWith(content.Get(), lineSep)) {
        content.Append(lineSep);
        coords.AppendBlanks(str::Len(lineSep));
    }

    if (coordsOut) {
        CrashIf(coords.size() != content.size());
        *coordsOut = coords.StealData();
    }
    return content.StealData();
}

void EbookEngine::UpdateUserAnnotations(Vec<PageAnnotation>* list) {
    ScopedCritSec scope(&pagesAccess);
    if (list) {
        userAnnots = *list;
    } else {
        userAnnots.Reset();
    }
}

PageElement* EbookEngine::CreatePageLink(DrawInstr* link, RectI rect, int pageNo) {
    AutoFreeWstr url(strconv::FromHtmlUtf8(link->str.s, link->str.len));
    if (url::IsAbsolute(url)) {
        return newEbookLink(link, rect, nullptr, pageNo);
    }

    DrawInstr* baseAnchor = baseAnchors.at(pageNo - 1);
    if (baseAnchor) {
        AutoFree basePath(str::DupN(baseAnchor->str.s, baseAnchor->str.len));
        AutoFree relPath(ResolveHtmlEntities(link->str.s, link->str.len));
        AutoFree absPath(NormalizeURL(relPath, basePath));
        url.Set(strconv::FromUtf8(absPath));
    }

    PageDestination* dest = GetNamedDest(url);
    if (!dest) {
        return nullptr;
    }
    return newEbookLink(link, rect, dest, pageNo);
}

Vec<PageElement*>* EbookEngine::GetElements(int pageNo) {
    Vec<PageElement*>* els = new Vec<PageElement*>();

    Vec<DrawInstr>* pageInstrs = GetHtmlPage(pageNo);
    for (DrawInstr& i : *pageInstrs) {
        if (DrawInstrType::Image == i.type) {
            auto box = GetInstrBbox(i, pageBorder);
            auto el = newImageDataElement(pageNo, &i.img, box);
            els->Append(el);
        } else if (DrawInstrType::LinkStart == i.type && !i.bbox.IsEmptyArea()) {
            PageElement* link = CreatePageLink(&i, GetInstrBbox(i, pageBorder), pageNo);
            if (link) {
                els->Append(link);
            }
        }
    }

    return els;
}

RenderedBitmap* EbookEngine::GetImageForPageElement(PageElement* el) {
    return nullptr;
}

PageElement* EbookEngine::GetElementAtPos(int pageNo, PointD pt) {
    Vec<PageElement*>* els = GetElements(pageNo);
    if (!els)
        return nullptr;

    PageElement* el = nullptr;
    for (size_t i = 0; i < els->size() && !el; i++)
        if (els->at(i)->GetRect().Contains(pt))
            el = els->at(i);

    if (el)
        els->Remove(el);
    DeleteVecMembers(*els);
    delete els;

    return el;
}

PageDestination* EbookEngine::GetNamedDest(const WCHAR* name) {
    AutoFree name_utf8(strconv::WstrToUtf8(name));
    const char* id = name_utf8.Get();
    if (str::FindChar(id, '#')) {
        id = str::FindChar(id, '#') + 1;
    }

    // if the name consists of both path and ID,
    // try to first skip to the page with the desired
    // path before looking for the ID to allow
    // for the same ID to be reused on different pages
    DrawInstr* baseAnchor = nullptr;
    int basePageNo = 0;
    if (id > name_utf8.Get() + 1) {
        size_t base_len = id - name_utf8.Get() - 1;
        for (size_t i = 0; i < baseAnchors.size(); i++) {
            DrawInstr* anchor = baseAnchors.at(i);
            if (anchor && base_len == anchor->str.len && str::EqNI(name_utf8.Get(), anchor->str.s, base_len)) {
                baseAnchor = anchor;
                basePageNo = (int)i + 1;
                break;
            }
        }
    }

    size_t id_len = str::Len(id);
    for (size_t i = 0; i < anchors.size(); i++) {
        PageAnchor* anchor = &anchors.at(i);
        if (baseAnchor) {
            if (anchor->instr == baseAnchor)
                baseAnchor = nullptr;
            continue;
        }
        // note: at least CHM treats URLs as case-independent
        if (id_len == anchor->instr->str.len && str::EqNI(id, anchor->instr->str.s, id_len)) {
            RectD rect(0, anchor->instr->bbox.Y + pageBorder, pageRect.dx, 10);
            rect.Inflate(-pageBorder, 0);
            return newSimpleDest(anchor->pageNo, rect);
        }
    }

    // don't fail if an ID doesn't exist in a merged document
    if (basePageNo != 0) {
        RectD rect(0, pageBorder, pageRect.dx, 10);
        rect.Inflate(-pageBorder, 0);
        return newSimpleDest(basePageNo, rect);
    }

    return nullptr;
}

WCHAR* EbookEngine::ExtractFontList() {
    ScopedCritSec scope(&pagesAccess);

    Vec<mui::CachedFont*> seenFonts;
    WStrVec fonts;

    for (int pageNo = 1; pageNo <= PageCount(); pageNo++) {
        Vec<DrawInstr>* pageInstrs = GetHtmlPage(pageNo);
        if (!pageInstrs)
            continue;

        for (DrawInstr& i : *pageInstrs) {
            if (DrawInstrType::SetFont != i.type || seenFonts.Contains(i.font))
                continue;
            seenFonts.Append(i.font);

            FontFamily family;
            if (!i.font->font) {
                // TODO: handle gdi
                CrashIf(!i.font->GetHFont());
                continue;
            }
            Status ok = i.font->font->GetFamily(&family);
            if (ok != Ok)
                continue;
            WCHAR fontName[LF_FACESIZE];
            ok = family.GetFamilyName(fontName);
            if (ok != Ok || fonts.FindI(fontName) != -1)
                continue;
            fonts.Append(str::Dup(fontName));
        }
    }
    if (fonts.size() == 0)
        return nullptr;

    fonts.SortNatural();
    return fonts.Join(L"\n");
}

static void AppendTocItem(EbookTocItem*& root, EbookTocItem* item, int level) {
    if (!root) {
        root = item;
        return;
    }
    // find the last child at each level, until finding the parent of the new item
    DocTocItem* r2 = root;
    while (--level > 0) {
        for (; r2->next; r2 = r2->next)
            ;
        if (r2->child)
            r2 = r2->child;
        else {
            r2->child = item;
            return;
        }
    }
    r2->AddSibling(item);
}

class EbookTocBuilder : public EbookTocVisitor {
    EngineBase* engine = nullptr;
    EbookTocItem* root = nullptr;
    int idCounter = 0;
    bool isIndex = false;

  public:
    explicit EbookTocBuilder(EngineBase* engine) {
        this->engine = engine;
    }

    void Visit(const WCHAR* name, const WCHAR* url, int level) override;

    EbookTocItem* GetRoot() {
        return root;
    }
    void SetIsIndex(bool value) {
        isIndex = value;
    }
};

void EbookTocBuilder::Visit(const WCHAR* name, const WCHAR* url, int level) {
    PageDestination* dest;
    if (!url) {
        dest = nullptr;
    } else if (url::IsAbsolute(url)) {
        dest = newSimpleDest(0, RectD(), str::Dup(url));
    } else {
        dest = engine->GetNamedDest(url);
        if (!dest && str::FindChar(url, '%')) {
            AutoFreeWstr decodedUrl(str::Dup(url));
            url::DecodeInPlace(decodedUrl);
            dest = engine->GetNamedDest(decodedUrl);
        }
    }

    EbookTocItem* item = new EbookTocItem(str::Dup(name), dest);
    item->id = ++idCounter;
    if (isIndex) {
        item->pageNo = 0;
        level++;
    }
    AppendTocItem(root, item, level);
}

/* EngineBase for handling EPUB documents */

class EpubEngineImpl : public EbookEngine {
  public:
    EpubEngineImpl();
    virtual ~EpubEngineImpl();
    EngineBase* Clone() override;

    std::string_view GetFileData() override;
    bool SaveFileAs(const char* copyFileName, bool includeUserAnnots = false) override;

    WCHAR* GetProperty(DocumentProperty prop) override {
        return prop != DocumentProperty::FontList ? doc->GetProperty(prop) : ExtractFontList();
    }

    DocTocTree* GetTocTree() override;

    static EngineBase* CreateFromFile(const WCHAR* fileName);
    static EngineBase* CreateFromStream(IStream* stream);

  protected:
    EpubDoc* doc = nullptr;
    IStream* stream = nullptr;
    DocTocTree* tocTree = nullptr;

    bool Load(const WCHAR* fileName);
    bool Load(IStream* stream);
    bool FinishLoading();
};

EpubEngineImpl::EpubEngineImpl() : EbookEngine() {
    kind = kindEngineEpub;
    defaultFileExt = L".epub";
}

EpubEngineImpl::~EpubEngineImpl() {
    delete doc;
    delete tocTree;
    if (stream) {
        stream->Release();
    }
}

EngineBase* EpubEngineImpl::Clone() {
    if (stream) {
        return CreateFromStream(stream);
    }
    if (FileName()) {
        return CreateFromFile(FileName());
    }
    return nullptr;
}

bool EpubEngineImpl::Load(const WCHAR* fileName) {
    this->fileName.SetCopy(fileName);
    if (dir::Exists(fileName)) {
        // load uncompressed documents as a recompressed ZIP stream
        ScopedComPtr<IStream> zipStream(OpenDirAsZipStream(fileName, true));
        if (!zipStream)
            return false;
        return Load(zipStream);
    }
    doc = EpubDoc::CreateFromFile(fileName);
    return FinishLoading();
}

bool EpubEngineImpl::Load(IStream* stream) {
    stream->AddRef();
    this->stream = stream;
    doc = EpubDoc::CreateFromStream(stream);
    return FinishLoading();
}

bool EpubEngineImpl::FinishLoading() {
    if (!doc) {
        return false;
    }

    HtmlFormatterArgs args{};
    args.htmlStr = doc->GetHtmlData();
    args.pageDx = (float)pageRect.dx - 2 * pageBorder;
    args.pageDy = (float)pageRect.dy - 2 * pageBorder;
    args.SetFontName(GetDefaultFontName());
    args.fontSize = GetDefaultFontSize();
    args.textAllocator = &allocator;
    args.textRenderMethod = mui::TextRenderMethodGdiplusQuick;

    pages = EpubFormatter(&args, doc).FormatAllPages(false);
    if (!ExtractPageAnchors())
        return false;

    if (doc->IsRTL()) {
        preferredLayout = (PageLayoutType)(Layout_Book | Layout_R2L);
    } else {
        preferredLayout = Layout_Book;
    }

    return pages->size() > 0;
}

std::string_view EpubEngineImpl::GetFileData() {
    return GetStreamOrFileData(stream, fileName);
}

bool EpubEngineImpl::SaveFileAs(const char* copyFileName, bool includeUserAnnots) {
    UNUSED(includeUserAnnots);
    AutoFreeWstr dstPath = strconv::Utf8ToWchar(copyFileName);

    if (stream) {
        AutoFree d = GetDataFromStream(stream, nullptr);
        bool ok = !d.empty() && file::WriteFile(dstPath, d.as_view());
        if (ok) {
            return true;
        }
    }
    if (!fileName) {
        return false;
    }
    return CopyFileW(fileName, dstPath, FALSE);
}

DocTocTree* EpubEngineImpl::GetTocTree() {
    if (tocTree) {
        return tocTree;
    }
    EbookTocBuilder builder(this);
    doc->ParseToc(&builder);
    EbookTocItem* root = builder.GetRoot();
    if (!root) {
        return nullptr;
    }
    tocTree = new DocTocTree(root);
    return tocTree;
}

EngineBase* EpubEngineImpl::CreateFromFile(const WCHAR* fileName) {
    EpubEngineImpl* engine = new EpubEngineImpl();
    if (!engine->Load(fileName)) {
        delete engine;
        return nullptr;
    }
    return engine;
}

EngineBase* EpubEngineImpl::CreateFromStream(IStream* stream) {
    EpubEngineImpl* engine = new EpubEngineImpl();
    if (!engine->Load(stream)) {
        delete engine;
        return nullptr;
    }
    return engine;
}

bool IsEpubEngineSupportedFile(const WCHAR* fileName, bool sniff) {
    if (sniff && dir::Exists(fileName)) {
        AutoFreeWstr mimetypePath(path::Join(fileName, L"mimetype"));
        return file::StartsWith(mimetypePath, "application/epub+zip");
    }
    return EpubDoc::IsSupportedFile(fileName, sniff);
}

EngineBase* CreateEpubEngineFromFile(const WCHAR* fileName) {
    return EpubEngineImpl::CreateFromFile(fileName);
}

EngineBase* CreateEpubEngineFromStream(IStream* stream) {
    return EpubEngineImpl::CreateFromStream(stream);
}

/* EngineBase for handling FictionBook2 documents */

class Fb2EngineImpl : public EbookEngine {
  public:
    Fb2EngineImpl() : EbookEngine() {
        kind = kindEngineFb2;
        defaultFileExt = L".fb2";
    }
    virtual ~Fb2EngineImpl() {
        delete tocTree;
        delete doc;
    }
    EngineBase* Clone() override {
        return fileName ? CreateFromFile(fileName) : nullptr;
    }

    WCHAR* GetProperty(DocumentProperty prop) override {
        return prop != DocumentProperty::FontList ? doc->GetProperty(prop) : ExtractFontList();
    }

    DocTocTree* GetTocTree() override;

    static EngineBase* CreateFromFile(const WCHAR* fileName);
    static EngineBase* CreateFromStream(IStream* stream);

  protected:
    Fb2Doc* doc = nullptr;
    DocTocTree* tocTree = nullptr;

    bool Load(const WCHAR* fileName);
    bool Load(IStream* stream);
    bool FinishLoading();
};

bool Fb2EngineImpl::Load(const WCHAR* fileName) {
    this->fileName.SetCopy(fileName);
    doc = Fb2Doc::CreateFromFile(fileName);
    return FinishLoading();
}

bool Fb2EngineImpl::Load(IStream* stream) {
    doc = Fb2Doc::CreateFromStream(stream);
    return FinishLoading();
}

bool Fb2EngineImpl::FinishLoading() {
    if (!doc) {
        return false;
    }

    HtmlFormatterArgs args;
    args.htmlStr = doc->GetXmlData();
    args.pageDx = (float)pageRect.dx - 2 * pageBorder;
    args.pageDy = (float)pageRect.dy - 2 * pageBorder;
    args.SetFontName(GetDefaultFontName());
    args.fontSize = GetDefaultFontSize();
    args.textAllocator = &allocator;
    args.textRenderMethod = mui::TextRenderMethodGdiplusQuick;

    if (doc->IsZipped()) {
        defaultFileExt = L".fb2z";
    }

    pages = Fb2Formatter(&args, doc).FormatAllPages(false);
    if (!ExtractPageAnchors()) {
        return false;
    }

    return pages->size() > 0;
}

DocTocTree* Fb2EngineImpl::GetTocTree() {
    if (tocTree) {
        return tocTree;
    }
    EbookTocBuilder builder(this);
    doc->ParseToc(&builder);
    EbookTocItem* root = builder.GetRoot();
    if (!root) {
        return nullptr;
    }
    tocTree = new DocTocTree(root);
    return tocTree;
}

EngineBase* Fb2EngineImpl::CreateFromFile(const WCHAR* fileName) {
    Fb2EngineImpl* engine = new Fb2EngineImpl();
    if (!engine->Load(fileName)) {
        delete engine;
        return nullptr;
    }
    return engine;
}

EngineBase* Fb2EngineImpl::CreateFromStream(IStream* stream) {
    Fb2EngineImpl* engine = new Fb2EngineImpl();
    if (!engine->Load(stream)) {
        delete engine;
        return nullptr;
    }
    return engine;
}

bool IsFb2EngineSupportedFile(const WCHAR* fileName, bool sniff) {
    return Fb2Doc::IsSupportedFile(fileName, sniff);
}

EngineBase* CreateFb2EngineFromFile(const WCHAR* fileName) {
    return Fb2EngineImpl::CreateFromFile(fileName);
}

EngineBase* CreateFb2EngineFromStream(IStream* stream) {
    return Fb2EngineImpl::CreateFromStream(stream);
}

/* EngineBase for handling Mobi documents */

#include "MobiDoc.h"

class MobiEngineImpl : public EbookEngine {
  public:
    MobiEngineImpl() : EbookEngine() {
        kind = kindEngineMobi;
        defaultFileExt = L".mobi";
    }
    ~MobiEngineImpl() override {
        delete tocTree;
        delete doc;
    }
    EngineBase* Clone() override {
        return fileName ? CreateFromFile(fileName) : nullptr;
    }

    WCHAR* GetProperty(DocumentProperty prop) override {
        return prop != DocumentProperty::FontList ? doc->GetProperty(prop) : ExtractFontList();
    }

    PageDestination* GetNamedDest(const WCHAR* name) override;
    DocTocTree* GetTocTree() override;

    static EngineBase* CreateFromFile(const WCHAR* fileName);
    static EngineBase* CreateFromStream(IStream* stream);

  protected:
    MobiDoc* doc = nullptr;
    DocTocTree* tocTree = nullptr;

    bool Load(const WCHAR* fileName);
    bool Load(IStream* stream);
    bool FinishLoading();
};

bool MobiEngineImpl::Load(const WCHAR* fileName) {
    this->fileName.SetCopy(fileName);
    doc = MobiDoc::CreateFromFile(fileName);
    return FinishLoading();
}

bool MobiEngineImpl::Load(IStream* stream) {
    doc = MobiDoc::CreateFromStream(stream);
    return FinishLoading();
}

bool MobiEngineImpl::FinishLoading() {
    if (!doc || PdbDocType::Mobipocket != doc->GetDocType()) {
        return false;
    }

    HtmlFormatterArgs args;
    args.htmlStr = doc->GetHtmlData();
    args.pageDx = (float)pageRect.dx - 2 * pageBorder;
    args.pageDy = (float)pageRect.dy - 2 * pageBorder;
    args.SetFontName(GetDefaultFontName());
    args.fontSize = GetDefaultFontSize();
    args.textAllocator = &allocator;
    args.textRenderMethod = mui::TextRenderMethodGdiplusQuick;

    pages = MobiFormatter(&args, doc).FormatAllPages();
    if (!ExtractPageAnchors()) {
        return false;
    }

    return pages->size() > 0;
}

PageDestination* MobiEngineImpl::GetNamedDest(const WCHAR* name) {
    int filePos = _wtoi(name);
    if (filePos < 0 || 0 == filePos && *name != '0') {
        return nullptr;
    }
    int pageNo;
    for (pageNo = 1; pageNo < PageCount(); pageNo++) {
        if (pages->at(pageNo)->reparseIdx > filePos) {
            break;
        }
    }
    CrashIf(pageNo < 1 || pageNo > PageCount());

    const std::string_view htmlData = doc->GetHtmlData();
    size_t htmlLen = htmlData.size();
    const char* start = htmlData.data();
    if ((size_t)filePos > htmlLen) {
        return nullptr;
    }

    ScopedCritSec scope(&pagesAccess);
    Vec<DrawInstr>* pageInstrs = GetHtmlPage(pageNo);
    // link to the bottom of the page, if filePos points
    // beyond the last visible DrawInstr of a page
    float currY = (float)pageRect.dy;
    for (DrawInstr& i : *pageInstrs) {
        if ((DrawInstrType::String == i.type || DrawInstrType::RtlString == i.type) && i.str.s >= start &&
            i.str.s <= start + htmlLen && i.str.s - start >= filePos) {
            currY = i.bbox.Y;
            break;
        }
    }
    RectD rect(0, currY + pageBorder, pageRect.dx, 10);
    rect.Inflate(-pageBorder, 0);
    return newSimpleDest(pageNo, rect);
}

DocTocTree* MobiEngineImpl::GetTocTree() {
    if (tocTree) {
        return tocTree;
    }
    EbookTocBuilder builder(this);
    doc->ParseToc(&builder);
    EbookTocItem* root = builder.GetRoot();
    if (!root) {
        return nullptr;
    }
    tocTree = new DocTocTree(root);
    return tocTree;
}

EngineBase* MobiEngineImpl::CreateFromFile(const WCHAR* fileName) {
    MobiEngineImpl* engine = new MobiEngineImpl();
    if (!engine->Load(fileName)) {
        delete engine;
        return nullptr;
    }
    return engine;
}

EngineBase* MobiEngineImpl::CreateFromStream(IStream* stream) {
    MobiEngineImpl* engine = new MobiEngineImpl();
    if (!engine->Load(stream)) {
        delete engine;
        return nullptr;
    }
    return engine;
}

bool IsMobiEngineSupportedFile(const WCHAR* fileName, bool sniff) {
    return MobiDoc::IsSupportedFile(fileName, sniff);
}

EngineBase* CreateMobiEngineFromFile(const WCHAR* fileName) {
    return MobiEngineImpl::CreateFromFile(fileName);
}

EngineBase* CreateMobiEngineFromStream(IStream* stream) {
    return MobiEngineImpl::CreateFromStream(stream);
}

/* EngineBase for handling PalmDOC documents (and extensions such as TealDoc) */

class PdbEngineImpl : public EbookEngine {
  public:
    PdbEngineImpl() : EbookEngine() {
        kind = kindEnginePdb;
        defaultFileExt = L".pdb";
    }
    virtual ~PdbEngineImpl() {
        delete tocTree;
        delete doc;
    }
    EngineBase* Clone() override {
        return fileName ? CreateFromFile(fileName) : nullptr;
    }

    WCHAR* GetProperty(DocumentProperty prop) override {
        return prop != DocumentProperty::FontList ? doc->GetProperty(prop) : ExtractFontList();
    }

    DocTocTree* GetTocTree() override;

    static EngineBase* CreateFromFile(const WCHAR* fileName);

  protected:
    PalmDoc* doc = nullptr;
    DocTocTree* tocTree = nullptr;

    bool Load(const WCHAR* fileName);
};

bool PdbEngineImpl::Load(const WCHAR* fileName) {
    this->fileName.SetCopy(fileName);

    doc = PalmDoc::CreateFromFile(fileName);
    if (!doc) {
        return false;
    }

    HtmlFormatterArgs args;
    args.htmlStr = doc->GetHtmlData();
    args.pageDx = (float)pageRect.dx - 2 * pageBorder;
    args.pageDy = (float)pageRect.dy - 2 * pageBorder;
    args.SetFontName(GetDefaultFontName());
    args.fontSize = GetDefaultFontSize();
    args.textAllocator = &allocator;
    args.textRenderMethod = mui::TextRenderMethodGdiplusQuick;

    pages = HtmlFormatter(&args).FormatAllPages();
    if (!ExtractPageAnchors()) {
        return false;
    }

    return pages->size() > 0;
}

DocTocTree* PdbEngineImpl::GetTocTree() {
    if (tocTree) {
        return tocTree;
    }
    EbookTocBuilder builder(this);
    doc->ParseToc(&builder);
    auto* root = builder.GetRoot();
    tocTree = new DocTocTree(root);
    return tocTree;
}

EngineBase* PdbEngineImpl::CreateFromFile(const WCHAR* fileName) {
    PdbEngineImpl* engine = new PdbEngineImpl();
    if (!engine->Load(fileName)) {
        delete engine;
        return nullptr;
    }
    return engine;
}

bool IsPdbEngineSupportedFile(const WCHAR* fileName, bool sniff) {
    return PalmDoc::IsSupportedFile(fileName, sniff);
}

EngineBase* CreatePdbEngineFromFile(const WCHAR* fileName) {
    return PdbEngineImpl::CreateFromFile(fileName);
}

/* formatting extensions for CHM */

#include "ChmDoc.h"

class ChmDataCache {
    ChmDoc* doc = nullptr; // owned by creator
    AutoFree html;
    Vec<ImageData2> images;

  public:
    ChmDataCache(ChmDoc* doc, char* html) : doc(doc), html(html) {
    }

    ~ChmDataCache() {
        for (size_t i = 0; i < images.size(); i++) {
            free(images.at(i).base.data);
            free(images.at(i).fileName);
        }
    }

    std::string_view GetHtmlData() {
        return html.as_view();
    }

    ImageData* GetImageData(const char* id, const char* pagePath) {
        AutoFree url(NormalizeURL(id, pagePath));
        for (size_t i = 0; i < images.size(); i++) {
            if (str::Eq(images.at(i).fileName, url)) {
                return &images.at(i).base;
            }
        }

        auto tmp = doc->GetData(url);
        if (tmp.empty()) {
            return nullptr;
        }

        ImageData2 data = {0};
        data.base.data = (char*)tmp.data();
        data.base.len = tmp.size();

        data.fileName = url.StealData();
        images.Append(data);
        return &images.Last().base;
    }

    std::string_view GetFileData(const char* relPath, const char* pagePath) {
        AutoFree url(NormalizeURL(relPath, pagePath));
        return doc->GetData(url);
    }
};

class ChmFormatter : public HtmlFormatter {
  protected:
    virtual void HandleTagImg(HtmlToken* t);
    virtual void HandleTagPagebreak(HtmlToken* t);
    virtual void HandleTagLink(HtmlToken* t);

    ChmDataCache* chmDoc = nullptr;
    AutoFree pagePath;

  public:
    ChmFormatter(HtmlFormatterArgs* args, ChmDataCache* doc) : HtmlFormatter(args), chmDoc(doc) {
    }
};

void ChmFormatter::HandleTagImg(HtmlToken* t) {
    CrashIf(!chmDoc);
    if (t->IsEndTag()) {
        return;
    }
    bool needAlt = true;
    AttrInfo* attr = t->GetAttrByName("src");
    if (attr) {
        AutoFree src(str::DupN(attr->val, attr->valLen));
        url::DecodeInPlace(src);
        ImageData* img = chmDoc->GetImageData(src, pagePath);
        needAlt = !img || !EmitImage(img);
    }
    if (needAlt && (attr = t->GetAttrByName("alt")) != nullptr) {
        HandleText(attr->val, attr->valLen);
    }
}

void ChmFormatter::HandleTagPagebreak(HtmlToken* t) {
    AttrInfo* attr = t->GetAttrByName("page_path");
    if (!attr || pagePath) {
        ForceNewPage();
    }
    if (attr) {
        RectF bbox(0, currY, pageDx, 0);
        currPage->instructions.Append(DrawInstr::Anchor(attr->val, attr->valLen, bbox));
        pagePath.Set(str::DupN(attr->val, attr->valLen));
        // reset CSS style rules for the new document
        styleRules.Reset();
    }
}

void ChmFormatter::HandleTagLink(HtmlToken* t) {
    CrashIf(!chmDoc);
    if (t->IsEndTag()) {
        return;
    }
    AttrInfo* attr = t->GetAttrByName("rel");
    if (!attr || !attr->ValIs("stylesheet")) {
        return;
    }
    attr = t->GetAttrByName("type");
    if (attr && !attr->ValIs("text/css")) {
        return;
    }
    attr = t->GetAttrByName("href");
    if (!attr) {
        return;
    }

    AutoFree src(str::DupN(attr->val, attr->valLen));
    url::DecodeInPlace(src);
    AutoFree data = chmDoc->GetFileData(src, pagePath);
    if (data.data) {
        ParseStyleSheet(data.data, data.size());
    }
}

/* EngineBase for handling CHM documents */

class ChmEngineImpl : public EbookEngine {
  public:
    ChmEngineImpl() : EbookEngine() {
        // ISO 216 A4 (210mm x 297mm)
        pageRect = RectD(0, 0, 8.27 * GetFileDPI(), 11.693 * GetFileDPI());
        kind = kindEngineChm;
        defaultFileExt = L".chm";
    }
    virtual ~ChmEngineImpl() {
        delete dataCache;
        delete doc;
        delete tocTree;
    }
    EngineBase* Clone() override {
        return fileName ? CreateFromFile(fileName) : nullptr;
    }

    WCHAR* GetProperty(DocumentProperty prop) override {
        return prop != DocumentProperty::FontList ? doc->GetProperty(prop) : ExtractFontList();
    }

    PageDestination* GetNamedDest(const WCHAR* name) override;
    DocTocTree* GetTocTree() override;

    static EngineBase* CreateFromFile(const WCHAR* fileName);

  protected:
    ChmDoc* doc = nullptr;
    ChmDataCache* dataCache = nullptr;
    DocTocTree* tocTree = nullptr;

    bool Load(const WCHAR* fileName);

    virtual PageElement* CreatePageLink(DrawInstr* link, RectI rect, int pageNo);
};

// cf. http://www.w3.org/TR/html4/charset.html#h-5.2.2
static UINT ExtractHttpCharset(const char* html, size_t htmlLen) {
    if (!strstr(html, "charset=")) {
        return 0;
    }

    HtmlPullParser parser(html, std::min(htmlLen, (size_t)1024));
    HtmlToken* tok;
    while ((tok = parser.Next()) != nullptr && !tok->IsError()) {
        if (tok->tag != Tag_Meta) {
            continue;
        }
        AttrInfo* attr = tok->GetAttrByName("http-equiv");
        if (!attr || !attr->ValIs("Content-Type")) {
            continue;
        }
        attr = tok->GetAttrByName("content");
        AutoFree mimetype, charset;
        if (!attr || !str::Parse(attr->val, attr->valLen, "%S;%_charset=%S", &mimetype, &charset)) {
            continue;
        }

        static struct {
            const char* name;
            UINT codepage;
        } codepages[] = {
            {"ISO-8859-1", 1252},  {"Latin1", 1252},   {"CP1252", 1252},   {"Windows-1252", 1252},
            {"ISO-8859-2", 28592}, {"Latin2", 28592},  {"CP1251", 1251},   {"Windows-1251", 1251},
            {"KOI8-R", 20866},     {"shift-jis", 932}, {"x-euc", 932},     {"euc-kr", 949},
            {"Big5", 950},         {"GB2312", 936},    {"UTF-8", CP_UTF8},
        };
        for (int i = 0; i < dimof(codepages); i++) {
            if (str::EqI(charset, codepages[i].name)) {
                return codepages[i].codepage;
            }
        }
        break;
    }

    return 0;
}

class ChmHtmlCollector : public EbookTocVisitor {
    ChmDoc* doc = nullptr;
    WStrList added;
    str::Str html;

  public:
    explicit ChmHtmlCollector(ChmDoc* doc) : doc(doc) {
        // can be big
        html.allowFailure = true;
    }

    char* GetHtml() {
        // first add the homepage
        const char* index = doc->GetHomePath();
        AutoFreeWstr url(doc->ToStr(index));
        Visit(nullptr, url, 0);

        // then add all pages linked to from the table of contents
        doc->ParseToc(this);

        // finally add all the remaining HTML files
        Vec<char*>* paths = doc->GetAllPaths();
        for (size_t i = 0; i < paths->size(); i++) {
            char* path = paths->at(i);
            if (str::EndsWithI(path, ".htm") || str::EndsWithI(path, ".html")) {
                if (*path == '/') {
                    path++;
                }
                url.Set(strconv::FromUtf8(path));
                Visit(nullptr, url, -1);
            }
        }
        paths->FreeMembers();
        delete paths;

        return html.StealData();
    }

    virtual void Visit(const WCHAR* name, const WCHAR* url, int level) {
        UNUSED(name);
        UNUSED(level);
        if (!url || url::IsAbsolute(url)) {
            return;
        }
        AutoFreeWstr plainUrl(url::GetFullPath(url));
        if (added.FindI(plainUrl) != -1) {
            return;
        }
        AutoFree urlUtf8(strconv::WstrToUtf8(plainUrl));
        AutoFree pageHtml = doc->GetData(urlUtf8.Get());
        if (!pageHtml) {
            return;
        }
        html.AppendFmt("<pagebreak page_path=\"%s\" page_marker />", urlUtf8.Get());
        auto charset = ExtractHttpCharset((const char*)pageHtml.Get(), pageHtml.size());
        html.AppendAndFree(doc->ToUtf8((const u8*)pageHtml.data, charset));
        added.Append(plainUrl.StealData());
    }
};

bool ChmEngineImpl::Load(const WCHAR* fileName) {
    this->fileName.SetCopy(fileName);
    doc = ChmDoc::CreateFromFile(fileName);
    if (!doc) {
        return false;
    }

    char* html = ChmHtmlCollector(doc).GetHtml();
    dataCache = new ChmDataCache(doc, html);

    HtmlFormatterArgs args;
    args.htmlStr = dataCache->GetHtmlData();
    args.pageDx = (float)pageRect.dx - 2 * pageBorder;
    args.pageDy = (float)pageRect.dy - 2 * pageBorder;
    args.SetFontName(GetDefaultFontName());
    args.fontSize = GetDefaultFontSize();
    args.textAllocator = &allocator;
    args.textRenderMethod = mui::TextRenderMethodGdiplusQuick;

    pages = ChmFormatter(&args, dataCache).FormatAllPages(false);
    if (!ExtractPageAnchors()) {
        return false;
    }

    return pages->size() > 0;
}

PageDestination* ChmEngineImpl::GetNamedDest(const WCHAR* name) {
    PageDestination* dest = EbookEngine::GetNamedDest(name);
    if (dest) {
        return dest;
    }
    unsigned int topicID;
    if (str::Parse(name, L"%u%$", &topicID)) {
        AutoFree urlUtf8(doc->ResolveTopicID(topicID));
        if (urlUtf8) {
            AutoFreeWstr url(strconv::FromUtf8(urlUtf8));
            dest = EbookEngine::GetNamedDest(url);
        }
    }
    return dest;
}

DocTocTree* ChmEngineImpl::GetTocTree() {
    if (tocTree) {
        return tocTree;
    }
    EbookTocBuilder builder(this);
    doc->ParseToc(&builder);
    if (doc->HasIndex()) {
        // TODO: ToC code doesn't work too well for displaying an index,
        //       so this should really become a tree of its own (which
        //       doesn't rely on entries being in the same order as pages)
        builder.Visit(L"Index", nullptr, 1);
        builder.SetIsIndex(true);
        doc->ParseIndex(&builder);
    }
    EbookTocItem* root = builder.GetRoot();
    if (!root) {
        return nullptr;
    }
    tocTree = new DocTocTree(root);
    return tocTree;
}

static PageDestination* newChmEmbeddedDest(const char* path) {
    auto res = new PageDestination();
    res->kind = kindDestinationLaunchEmbedded;
    res->value = strconv::Utf8ToWchar(path::GetBaseNameNoFree(path));
    return res;
}

PageElement* ChmEngineImpl::CreatePageLink(DrawInstr* link, RectI rect, int pageNo) {
    PageElement* linkEl = EbookEngine::CreatePageLink(link, rect, pageNo);
    if (linkEl) {
        return linkEl;
    }

    DrawInstr* baseAnchor = baseAnchors.at(pageNo - 1);
    AutoFree basePath(str::DupN(baseAnchor->str.s, baseAnchor->str.len));
    AutoFree url(str::DupN(link->str.s, link->str.len));
    url.Set(NormalizeURL(url, basePath));
    if (!doc->HasData(url)) {
        return nullptr;
    }

    PageDestination* dest = newChmEmbeddedDest(url);
    return newEbookLink(link, rect, dest, pageNo);
}

EngineBase* ChmEngineImpl::CreateFromFile(const WCHAR* fileName) {
    ChmEngineImpl* engine = new ChmEngineImpl();
    if (!engine->Load(fileName)) {
        delete engine;
        return nullptr;
    }
    return engine;
}

bool IsChmEngineSupportedFile(const WCHAR* fileName, bool sniff) {
    return ChmDoc::IsSupportedFile(fileName, sniff);
}

EngineBase* CreateChmEngineFromFile(const WCHAR* fileName) {
    return ChmEngineImpl::CreateFromFile(fileName);
}

/* EngineBase for handling HTML documents */
/* (mainly to allow creating minimal regression test testcases more easily) */

class HtmlEngineImpl : public EbookEngine {
  public:
    HtmlEngineImpl() : EbookEngine() {
        // ISO 216 A4 (210mm x 297mm)
        pageRect = RectD(0, 0, 8.27 * GetFileDPI(), 11.693 * GetFileDPI());
        defaultFileExt = L".html";
    }
    virtual ~HtmlEngineImpl() {
        delete doc;
    }
    EngineBase* Clone() override {
        return fileName ? CreateFromFile(fileName) : nullptr;
    }

    WCHAR* GetProperty(DocumentProperty prop) override {
        return prop != DocumentProperty::FontList ? doc->GetProperty(prop) : ExtractFontList();
    }

    static EngineBase* CreateFromFile(const WCHAR* fileName);

  protected:
    HtmlDoc* doc = nullptr;

    bool Load(const WCHAR* fileName);

    virtual PageElement* CreatePageLink(DrawInstr* link, RectI rect, int pageNo);
};

bool HtmlEngineImpl::Load(const WCHAR* fileName) {
    this->fileName.SetCopy(fileName);

    doc = HtmlDoc::CreateFromFile(fileName);
    if (!doc) {
        return false;
    }

    HtmlFormatterArgs args;
    args.htmlStr = doc->GetHtmlData();
    args.pageDx = (float)pageRect.dx - 2 * pageBorder;
    args.pageDy = (float)pageRect.dy - 2 * pageBorder;
    args.SetFontName(GetDefaultFontName());
    args.fontSize = GetDefaultFontSize();
    args.textAllocator = &allocator;
    args.textRenderMethod = mui::TextRenderMethodGdiplus;

    pages = HtmlFileFormatter(&args, doc).FormatAllPages(false);
    if (!ExtractPageAnchors()) {
        return false;
    }

    return pages->size() > 0;
}

static PageDestination* newRemoteHtmlDest(const WCHAR* relativeURL) {
    auto* res = new PageDestination();
    const WCHAR* id = str::FindChar(relativeURL, '#');
    if (id) {
        res->value = str::DupN(relativeURL, id - relativeURL);
        res->name = str::Dup(id);
    } else {
        res->value = str::Dup(relativeURL);
    }
    res->kind = kindDestinationLaunchFile;
    return res;
}

PageElement* HtmlEngineImpl::CreatePageLink(DrawInstr* link, RectI rect, int pageNo) {
    if (0 == link->str.len) {
        return nullptr;
    }

    AutoFreeWstr url(strconv::FromHtmlUtf8(link->str.s, link->str.len));
    if (url::IsAbsolute(url) || '#' == *url) {
        return EbookEngine::CreatePageLink(link, rect, pageNo);
    }

    PageDestination* dest = newRemoteHtmlDest(url);
    return newEbookLink(link, rect, dest, pageNo, true);
}

EngineBase* HtmlEngineImpl::CreateFromFile(const WCHAR* fileName) {
    HtmlEngineImpl* engine = new HtmlEngineImpl();
    if (!engine->Load(fileName)) {
        delete engine;
        return nullptr;
    }
    return engine;
}

bool IsHtmlEngineSupportedFile(const WCHAR* fileName, bool sniff) {
    return HtmlDoc::IsSupportedFile(fileName, sniff);
}

EngineBase* CreateHtmlEngineFromFile(const WCHAR* fileName) {
    return HtmlEngineImpl::CreateFromFile(fileName);
}

/* EngineBase for handling TXT documents */

class TxtEngineImpl : public EbookEngine {
  public:
    TxtEngineImpl() : EbookEngine() {
        kind = kindEngineTxt;
        // ISO 216 A4 (210mm x 297mm)
        pageRect = RectD(0, 0, 8.27 * GetFileDPI(), 11.693 * GetFileDPI());
        defaultFileExt = L".txt";
    }
    virtual ~TxtEngineImpl() {
        delete tocTree;
        delete doc;
    }
    EngineBase* Clone() override {
        return fileName ? CreateFromFile(fileName) : nullptr;
    }

    WCHAR* GetProperty(DocumentProperty prop) override {
        return prop != DocumentProperty::FontList ? doc->GetProperty(prop) : ExtractFontList();
    }

    DocTocTree* GetTocTree() override;

    static EngineBase* CreateFromFile(const WCHAR* fileName);

  protected:
    TxtDoc* doc = nullptr;
    DocTocTree* tocTree = nullptr;

    bool Load(const WCHAR* fileName);
};

bool TxtEngineImpl::Load(const WCHAR* fileName) {
    if (!fileName) {
        return false;
    }

    this->fileName.SetCopy(fileName);

    defaultFileExt = path::GetExt(fileName);

    doc = TxtDoc::CreateFromFile(fileName);
    if (!doc)
        return false;

    if (doc->IsRFC()) {
        // RFCs are targeted at letter size pages
        pageRect = RectD(0, 0, 8.5 * GetFileDPI(), 11 * GetFileDPI());
    }

    HtmlFormatterArgs args;
    args.htmlStr = doc->GetHtmlData();
    args.pageDx = (float)pageRect.dx - 2 * pageBorder;
    args.pageDy = (float)pageRect.dy - 2 * pageBorder;
    args.SetFontName(GetDefaultFontName());
    args.fontSize = GetDefaultFontSize();
    args.textAllocator = &allocator;
    args.textRenderMethod = mui::TextRenderMethodGdiplus;

    pages = TxtFormatter(&args).FormatAllPages(false);
    if (!ExtractPageAnchors())
        return false;

    return pages->size() > 0;
}

DocTocTree* TxtEngineImpl::GetTocTree() {
    if (tocTree) {
        return tocTree;
    }
    EbookTocBuilder builder(this);
    doc->ParseToc(&builder);
    auto* root = builder.GetRoot();

    tocTree = new DocTocTree(root);
    return tocTree;
}

EngineBase* TxtEngineImpl::CreateFromFile(const WCHAR* fileName) {
    TxtEngineImpl* engine = new TxtEngineImpl();
    if (!engine->Load(fileName)) {
        delete engine;
        return nullptr;
    }
    return engine;
}

bool IsTxtEngineSupportedFile(const WCHAR* fileName, bool sniff) {
    return TxtDoc::IsSupportedFile(fileName, sniff);
}

EngineBase* CreateTxtEngineFromFile(const WCHAR* fileName) {
    return TxtEngineImpl::CreateFromFile(fileName);
}
