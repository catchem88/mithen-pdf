/* Copyright 2026 the MithenPDF project authors. See AUTHORS file. */

// Asks a Windows Explorer window showing `dir` for the order its items are
// displayed in (the folder's current sorting) and appends the paths to `out`.
// Returns false when no Explorer window shows the folder; `out` is untouched.
bool GetExplorerSortOrder(Str dir, StrVec& out);

// The order an Explorer window shows `dir`'s items in, looked up by file name.
// Null when no Explorer window shows the folder. Free with FreeExplorerOrder().
struct ExplorerOrder;
ExplorerOrder* GetExplorerOrder(Str dir);
void FreeExplorerOrder(ExplorerOrder* order);

// Position Explorer shows the file named `name` at, or -1 when it doesn't show
// it (also -1 for a null `order`).
int ExplorerOrderRank(const ExplorerOrder* order, Str name);

// Reorders `paths` - files all in one folder - into the order Explorer displays
// them in: whatever sort column and direction the user set there (Name, Date
// modified, Date created, Type, Size, ...). Entries Explorer doesn't show keep
// their natural order, after the ranked ones. No-op when the paths span folders
// or no Explorer window shows the folder.
void SortPathsByExplorerOrder(StrVec& paths);
