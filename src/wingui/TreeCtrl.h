/* Copyright 2019 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

struct TreeModel;
struct TreeItem;
class TreeCtrl;

typedef std::function<LRESULT(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, bool& discardMsg)> MsgFilter;
typedef std::function<void(TreeCtrl*, NMTVGETINFOTIPW*)> OnGetInfoTip;
typedef std::function<LRESULT(TreeCtrl*, NMTREEVIEWW*, bool&)> OnTreeNotify;

// function called for every item in the tree.
// returning false stops iteration
typedef std::function<bool(TVITEM*)> TreeItemVisitor;


/* Creation sequence:
- auto ctrl = new TreeCtrl()
- set creation parameters
- ctrl->Create()
*/

class TreeCtrl {
  public:
    TreeCtrl(HWND parent, RECT* initialPosition);
    ~TreeCtrl();

    void Clear();
    TVITEMW* GetItem(HTREEITEM);
    std::wstring GetInfoTip(HTREEITEM);
    HTREEITEM GetRoot();
    HTREEITEM GetChild(HTREEITEM);
    HTREEITEM GetSiblingNext(HTREEITEM); // GetNextSibling is windows macro
    HTREEITEM GetSelection();
    bool SelectItem(HTREEITEM);
    HTREEITEM InsertItem(TV_INSERTSTRUCT*);

    void VisitNodes(const TreeItemVisitor& visitor);
    // TODO: create 2 functions for 2 different fItemRect values
    bool GetItemRect(HTREEITEM, bool fItemRect, RECT& r);
    bool IsExpanded(HTREEITEM);

    bool Create(const WCHAR* title);
    void SetFont(HFONT);
    void SetTreeModel(TreeModel*);

    void SuspendRedraw();
    void ResumeRedraw();

    HTREEITEM GetHandleByTreeItem(TreeItem*);
    TreeItem* GetTreeItemByHandle(HTREEITEM);


    // creation parameters. must be set before CreateTreeCtrl() call
    HWND parent = nullptr;
    RECT initialPos = {0, 0, 0, 0};
    DWORD dwStyle = WS_CHILD | WS_VISIBLE | WS_TABSTOP | TVS_HASBUTTONS | TVS_HASLINES | TVS_LINESATROOT |
                    TVS_SHOWSELALWAYS | TVS_TRACKSELECT | TVS_DISABLEDRAGDROP | TVS_NOHSCROLL | TVS_INFOTIP;
    DWORD dwExStyle = 0;
    HMENU menu = nullptr;
    COLORREF bgCol = 0;
    TreeModel *treeModel = nullptr; // not owned by us
    WCHAR infotipBuf[INFOTIPSIZE + 1]; // +1 just in case

    // this data can be set directly
    MsgFilter preFilter; // called at start of windows proc to allow intercepting messages

    // when set, allows the caller to set info tip by updating NMTVGETINFOTIP
    OnGetInfoTip onGetInfoTip;

    // if set, called to process all WM_NOTIFY messages
    OnTreeNotify onTreeNotify;

    // private
    HWND hwnd = nullptr;
    TVITEMW item = {0};
    UINT_PTR hwndSubclassId = 0;
    UINT_PTR hwndParentSubclassId = 0;

    // TreeItem* -> HTREEITEM mapping so that we can
    // find HTREEITEM from TreeItem*
    std::vector<std::tuple<TreeItem*,HTREEITEM>> insertedItems;
};

void TreeViewExpandRecursively(HWND hTree, HTREEITEM hItem, UINT flag, bool subtree);
