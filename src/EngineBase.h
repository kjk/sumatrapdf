/* Copyright 2015 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

extern Kind kindEnginePdf;
extern Kind kindEnginePdfMulti;
extern Kind kindEngineXps;
extern Kind kindEngineDjVu;
extern Kind kindEngineImage;
extern Kind kindEngineImageDir;
extern Kind kindEngineComicBooks;
extern Kind kindEnginePostScript;
extern Kind kindEngineEpub;
extern Kind kindEngineFb2;
extern Kind kindEngineMobi;
extern Kind kindEnginePdb;
extern Kind kindEngineChm;
extern Kind kindEngineHtml;
extern Kind kindEngineTxt;

/* certain OCGs will only be rendered for some of these (e.g. watermarks) */
enum class RenderTarget { View, Print, Export };

enum PageLayoutType {
    Layout_Single = 0,
    Layout_Facing = 1,
    Layout_Book = 2,
    Layout_R2L = 16,
    Layout_NonContinuous = 32
};

enum class PageElementType { Link, Image, Comment };


enum class PageAnnotType {
    None,
    Highlight,
    Underline,
    StrikeOut,
    Squiggly,
};

enum class DocumentProperty {
    Title,
    Author,
    Copyright,
    Subject,
    CreationDate,
    ModificationDate,
    CreatorApp,
    UnsupportedFeatures,
    FontList,
    PdfVersion,
    PdfProducer,
    PdfFileStructure,
};

class RenderedBitmap {
  public:
    HBITMAP hbmp = nullptr;
    SizeI size = {};
    ScopedHandle hMap = {};

    RenderedBitmap(HBITMAP hbmp, SizeI size, HANDLE hMap = nullptr) : hbmp(hbmp), size(size), hMap(hMap) {
    }
    ~RenderedBitmap();
    RenderedBitmap* Clone() const;
    HBITMAP GetBitmap() const;
    SizeI Size() const;
    bool StretchDIBits(HDC hdc, RectI target) const;
};


enum class PageDestType {
    None,
    ScrollTo,
    LaunchURL,
    LaunchEmbedded,
    LaunchFile,
    NextPage,
    PrevPage,
    FirstPage,
    LastPage,
    FindDialog,
    FullScreen,
    GoBack,
    GoForward,
    GoToPageDialog,
    PrintDialog,
    SaveAsDialog,
    ZoomToDialog,
};

// a link destination
class PageDestination {
  public:
    PageDestType destType = PageDestType::None;

    virtual ~PageDestination() {
    }

    PageDestType GetDestType() const {
        return destType;
    }

    // page the destination points to (0 for external destinations such as URLs)
    virtual int GetDestPageNo() const = 0;
    // rectangle of the destination on the above returned page
    virtual RectD GetDestRect() const = 0;
    // string value associated with the destination (e.g. a path or a URL)
    // caller must free() the result
    virtual WCHAR* GetDestValue() const {
        return nullptr;
    }
    // the name of this destination (reverses EngineBase::GetNamedDest) or nullptr
    // (mainly applicable for links of type "LaunchFile" to PDF documents)
    // caller must free() the result
    virtual WCHAR* GetDestName() const {
        return nullptr;
    }
};

// an user annotation on page
struct PageAnnotation {
    PageAnnotType type = PageAnnotType::None;
    int pageNo = -1;
    RectD rect = {};
    COLORREF color = 0;

    PageAnnotation() = default;

    PageAnnotation(PageAnnotType type, int pageNo, RectD rect, COLORREF color)
        : type(type), pageNo(pageNo), rect(rect), color(color) {
    }
    bool operator==(const PageAnnotation& other) const {
        return other.type == type && other.pageNo == pageNo && other.rect == rect && other.color == color;
    }
};

// use in PageDestination::GetDestRect for values that don't matter
#define DEST_USE_DEFAULT -999.9

// hoverable (and maybe interactable) element on a single page
class PageElement {
  public:
    int pageNo = -1;

    virtual ~PageElement() {
    }
    // the type of this page element
    virtual PageElementType GetType() const = 0;

    // page this element lives on (0 for elements in a ToC)
    int GetPageNo() const {
        return pageNo;
    }

    // rectangle that can be interacted with
    virtual RectD GetRect() const = 0;
    // string value associated with this element (e.g. displayed in an infotip)
    // caller must free() the result
    virtual WCHAR* GetValue() const = 0;

    // if this element is a link, this returns information about the link's destination
    // (the result is owned by the PageElement and MUST NOT be deleted)
    virtual PageDestination* AsLink() {
        return nullptr;
    }
    // if this element is an image, this returns it
    // caller must delete the result
    virtual RenderedBitmap* GetImage() {
        return nullptr;
    }
};

// those are the same as F font bitmask in PDF docs
// for DocTocItem::fontFlags
constexpr int fontBitBold = 0;
constexpr int fontBitItalic = 1;

// an item in a document's Table of Content
class DocTocItem : public TreeItem {
  public:
    // the item's visible label
    WCHAR* title = nullptr;

    // in some formats, the document can specify the tree item
    // is expanded by default. We keep track if user toggled
    // expansion state of the tree item
    bool isOpenDefault = false;
    bool isOpenToggled = false;

    // page this item points to (0 for non-page destinations)
    // if GetLink() returns a destination to a page, the two should match
    int pageNo = 0;
    // arbitrary number allowing to distinguish this DocTocItem
    // from any other of the same ToC tree (must be constant
    // between runs so that it can be persisted in FileState::tocState)
    int id = 0;

    int fontFlags = 0; // fontBitBold, fontBitItalic
    COLORREF color = ColorUnset;

    // first child item
    DocTocItem* child = nullptr;
    // next sibling
    DocTocItem* next = nullptr;

    DocTocItem() = default;

    explicit DocTocItem(WCHAR* title, int pageNo = 0);

    virtual ~DocTocItem();

    void AddSibling(DocTocItem* sibling);

    void OpenSingleNode();

    virtual PageDestination* GetLink();

    WCHAR* Text();

    // TreeItem
    TreeItem* Parent();
    int ChildCount();
    TreeItem* ChildAt(int n);
    bool IsExpanded();
};

struct DocTocTree : public TreeModel {
    // name of the bookmark view
    char* name = nullptr;
    DocTocItem* root = nullptr;

    DocTocTree() = default;
    DocTocTree(DocTocItem* root);
    virtual ~DocTocTree();

    // TreeModel
    int RootCount();
    TreeItem* RootAt(int n);
};

// a helper that allows for rendering interruptions in an engine-agnostic way
class AbortCookie {
  public:
    virtual ~AbortCookie() {
    }
    // aborts a rendering request (as far as possible)
    // note: must be thread-safe
    virtual void Abort() = 0;
};

class EngineBase {
  public:
    Kind kind = nullptr;
    // the default file extension for a document like
    // the currently loaded one (e.g. L".pdf")
    const WCHAR* defaultFileExt = nullptr;
    PageLayoutType preferredLayout = Layout_Single;
    float fileDPI = 96.0f;
    bool isImageCollection = false;
    bool allowsPrinting = true;
    bool allowsCopyingText = true;
    bool isPasswordProtected = false;
    char* decryptionKey = nullptr;
    bool hasPageLabels = false;

    virtual ~EngineBase() {
        free(decryptionKey);
    }
    // creates a clone of this engine (e.g. for printing on a different thread)
    virtual EngineBase* Clone() = 0;

    // number of pages the loaded document contains
    virtual int PageCount() const = 0;

    // the box containing the visible page content (usually RectD(0, 0, pageWidth, pageHeight))
    virtual RectD PageMediabox(int pageNo) = 0;
    // the box inside PageMediabox that actually contains any relevant content
    // (used for auto-cropping in Fit Content mode, can be PageMediabox)
    virtual RectD PageContentBox(int pageNo, RenderTarget target = RenderTarget::View) {
        UNUSED(target);
        return PageMediabox(pageNo);
    }

    // renders a page into a cacheable RenderedBitmap
    // (*cookie_out must be deleted after the call returns)
    virtual RenderedBitmap* RenderBitmap(int pageNo, float zoom, int rotation,
                                         RectD* pageRect = nullptr, /* if nullptr: defaults to the page's mediabox */
                                         RenderTarget target = RenderTarget::View,
                                         AbortCookie** cookie_out = nullptr) = 0;

    // applies zoom and rotation to a point in user/page space converting
    // it into device/screen space - or in the inverse direction
    virtual PointD Transform(PointD pt, int pageNo, float zoom, int rotation, bool inverse = false) = 0;
    virtual RectD Transform(RectD rect, int pageNo, float zoom, int rotation, bool inverse = false) = 0;

    // returns the binary data for the current file
    // (e.g. for saving again when the file has already been deleted)
    // caller needs to free() the result
    virtual std::string_view GetFileData() = 0;

    // saves a copy of the current file under a different name (overwriting an existing file)
    // (includeUserAnnots only has an effect if SupportsAnnotation(true) returns true)
    virtual bool SaveFileAs(const char* copyFileName, bool includeUserAnnots = false) = 0;
    // converts the current file to a PDF file and saves it (overwriting an existing file),
    // (includeUserAnnots should always have an effect)
    virtual bool SaveFileAsPDF(const char* pdfFileName, bool includeUserAnnots = false) {
        UNUSED(pdfFileName);
        UNUSED(includeUserAnnots);
        return false;
    }
    // extracts all text found in the given page (and optionally also the
    // coordinates of the individual glyphs)
    // caller needs to free() the result and *coordsOut (if coordsOut is non-nullptr)
    virtual WCHAR* ExtractPageText(int pageNo, RectI** coordsOut = nullptr) = 0;
    // pages where clipping doesn't help are rendered in larger tiles
    virtual bool HasClipOptimizations(int pageNo) = 0;
    // the layout type this document's author suggests (if the user doesn't care)
    // whether the content should be displayed as images instead of as document pages
    // (e.g. with a black background and less padding in between and without search UI)
    bool IsImageCollection() const {
        return isImageCollection;
    }

    // access to various document properties (such as Author, Title, etc.)
    virtual WCHAR* GetProperty(DocumentProperty prop) = 0;

    // TODO: generalize from PageAnnotation to PageModification
    // whether this engine supports adding user annotations of all available types
    // (either for rendering or for saving)
    virtual bool SupportsAnnotation(bool forSaving = false) const = 0;
    // informs the engine about annotations the user made so that they can be rendered, etc.
    // (this call supercedes any prior call to UpdateUserAnnotations)
    virtual void UpdateUserAnnotations(Vec<PageAnnotation>* list) = 0;

    // TODO: needs a more general interface
    // whether it is allowed to print the current document
    bool AllowsPrinting() const {
        return allowsPrinting;
    }
    // whether it is allowed to extract text from the current document
    // (except for searching an accessibility reasons)
    bool AllowsCopyingText() const {
        return allowsCopyingText;
    }

    // the DPI for a file is needed when converting internal measures to physical ones
    float GetFileDPI() const {
        return fileDPI;
    }

    // returns a list of all available elements for this page
    // caller must delete the result (including all elements contained in the Vec)
    virtual Vec<PageElement*>* GetElements(int pageNo) = 0;
    // returns the element at a given point or nullptr if there's none
    // caller must delete the result
    virtual PageElement* GetElementAtPos(int pageNo, PointD pt) = 0;

    // creates a PageDestination from a name (or nullptr for invalid names)
    // caller must delete the result
    virtual PageDestination* GetNamedDest(const WCHAR* name) {
        UNUSED(name);
        return nullptr;
    }
    // checks whether this document has an associated Table of Contents
    bool HasTocTree() {
        DocTocTree* tree = GetTocTree();
        return tree != nullptr;
    }
    // returns the root element for the loaded document's Table of Contents
    // caller must delete the result (when no longer needed)
    virtual DocTocTree* GetTocTree() {
        return nullptr;
    }

    // checks whether this document has explicit labels for pages (such as
    // roman numerals) instead of the default plain arabic numbering
    bool HasPageLabels() const {
        return hasPageLabels;
    }
    // returns a label to be displayed instead of the page number
    // caller must free() the result
    virtual WCHAR* GetPageLabel(int pageNo) const {
        return str::Format(L"%d", pageNo);
    }
    // reverts GetPageLabel by returning the first page number having the given label
    virtual int GetPageByLabel(const WCHAR* label) const {
        return _wtoi(label);
    }

    // whether this document required a password in order to be loaded
    bool IsPasswordProtected() const {
        return isPasswordProtected;
    }
    // returns a string to remember when the user wants to save a document's password
    // (don't implement for document types that don't support password protection)
    // caller must free() the result
    char* GetDecryptionKey() const {
        return str::Dup(decryptionKey);
    }

    // loads the given page so that the time required can be measured
    // without also measuring rendering times
    virtual bool BenchLoadPage(int pageNo) = 0;

    // the name of the file this engine handles
    const WCHAR* FileName() const {
        return fileName.Get();
    }

  protected:
    void SetFileName(const WCHAR* s) {
        fileName.SetCopy(s);
    }

    AutoFreeWstr fileName;
};

class PasswordUI {
  public:
    virtual WCHAR* GetPassword(const WCHAR* fileName, unsigned char* fileDigest, unsigned char decryptionKeyOut[32],
                               bool* saveKey) = 0;
    virtual ~PasswordUI() {
    }
};
