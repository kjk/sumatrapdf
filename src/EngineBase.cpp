/* Copyright 2019 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "utils/BaseUtil.h"
#include "utils/ScopedWin.h"
#include "utils/WinUtil.h"

#include "wingui/TreeModel.h"

#include "EngineBase.h"

Kind kindPageElementDest = "dest";
Kind kindPageElementImage = "image";
Kind kindPageElementComment = "comment";

Kind kindDestinationScrollTo = "scrollTo";
Kind kindDestinationLaunchURL = "launchURL";
Kind kindDestinationLaunchEmbedded = "launchEmbedded";
Kind kindDestinationLaunchFile = "launchFile";
Kind kindDestinationNextPage = "nextPage";
Kind kindDestinationPrevPage = "prevPage";
Kind kindDestinationFirstPage = "firstPage";
Kind kindDestinationLastPage = "lastPage";
Kind kindDestinationFindDialog = "findDialog";
Kind kindDestinationFullScreen = "fullscreen";
Kind kindDestinationGoBack = "goBack";
Kind kindDestinationGoForward = "goForward";
Kind kindDestinationGoToPageDialog = "goToPageDialog";
Kind kindDestinationPrintDialog = "printDialog";
Kind kindDestinationSaveAsDialog = "saveAsDialog";
Kind kindDestinationZoomToDialog = "zoomToDialog";

RenderedBitmap::~RenderedBitmap() {
    DeleteObject(hbmp);
}

RenderedBitmap* RenderedBitmap::Clone() const {
    HBITMAP hbmp2 = (HBITMAP)CopyImage(hbmp, IMAGE_BITMAP, size.dx, size.dy, 0);
    return new RenderedBitmap(hbmp2, size);
}

// render the bitmap into the target rectangle (streching and skewing as requird)
bool RenderedBitmap::StretchDIBits(HDC hdc, RectI target) const {
    return BlitHBITMAP(hbmp, hdc, target);
}

// callers must not delete this (use Clone if you have to modify it)
HBITMAP RenderedBitmap::GetBitmap() const {
    return hbmp;
}

SizeI RenderedBitmap::Size() const {
    return size;
}

DocTocItem::DocTocItem(WCHAR* title, int pageNo) {
    this->title = title;
    this->pageNo = pageNo;
}

DocTocItem::~DocTocItem() {
    delete child;
    while (next) {
        DocTocItem* tmp = next->next;
        next->next = nullptr;
        delete next;
        next = tmp;
    }
    free(title);
}

void DocTocItem::AddSibling(DocTocItem* sibling) {
    DocTocItem* item = this;
    while (item->next) {
        item = item->next;
    }
    item->next = sibling;
}

void DocTocItem::OpenSingleNode() {
    // only open (root level) ToC nodes if there's at most two
    if (next && next->next) {
        return;
    }

    if (!IsExpanded()) {
        isOpenToggled = !isOpenToggled;
    }
    if (!next) {
        return;
    }
    if (!next->IsExpanded()) {
        next->isOpenToggled = !next->isOpenToggled;
    }
}

// returns the destination this ToC item points to or nullptr
// (the result is owned by the DocTocItem and MUST NOT be deleted)
// TODO: change this to char* uri that encodes all the information
// about PageDestination, get rid of PageDestination and all
// classes that inherit from DocTocItem
// virtual PageDestination* GetLink() = 0;
PageDestination* DocTocItem::GetPageDestination() {
    return nullptr;
}

WCHAR* DocTocItem::Text() {
    return title;
}

TreeItem* DocTocItem::Parent() {
    // don't use it
    CrashMe();
    return nullptr;
}

int DocTocItem::ChildCount() {
    int n = 0;
    auto node = child;
    while (node) {
        n++;
        node = node->next;
    }
    return n;
}

TreeItem* DocTocItem::ChildAt(int n) {
    auto node = child;
    while (n > 0) {
        node = node->next;
        n--;
    }
    return node;
}

bool DocTocItem::IsExpanded() {
    // leaf items cannot be expanded
    if (child == nullptr) {
        return false;
    }
    // item is expanded when:
    // - expanded by default, not toggled (true, false)
    // - not expanded by default, toggled (false, true)
    // which boils down to:
    return isOpenDefault != isOpenToggled;
}

DocTocTree::DocTocTree(DocTocItem* root) {
    this->root = root;
}

DocTocTree::~DocTocTree() {
    free(name);
    delete root;
}

int DocTocTree::RootCount() {
    int n = 0;
    auto node = root;
    while (node) {
        n++;
        node = node->next;
    }
    return n;
}

TreeItem* DocTocTree::RootAt(int n) {
    auto node = root;
    while (n > 0) {
        node = node->next;
        n--;
    }
    return node;
}
