#include "navigation.h"
#include <qpdf/QPDFPageDocumentHelper.hh>
#include <algorithm>
#include <set>

namespace pdfengine::nav {
namespace {
using Obj = QPDFObjectHandle;
constexpr std::size_t maxItems = 20000;
constexpr int maxDepth = 32;

struct Node { Obj item; int level; };

void walk(Obj first, int level, std::set<QPDFObjGen>& seen, std::vector<Node>& out) {
    for (auto item = first; item.isDictionary() && out.size() < maxItems; item = item.getKey("/Next")) {
        if (item.isIndirect() && !seen.insert(item.getObjGen()).second) break;
        out.push_back({item, level});
        auto child = item.getKey("/First");
        if (child.isDictionary() && level + 1 < maxDepth) walk(child, level + 1, seen, out);
    }
}
std::vector<Node> flatten(QPDF& pdf) {
    std::vector<Node> out; std::set<QPDFObjGen> seen;
    auto root = pdf.getRoot().getKey("/Outlines");
    if (root.isDictionary()) walk(root.getKey("/First"), 0, seen, out);
    return out;
}
Obj destArray(Obj item) {
    auto dest = item.getKey("/Dest");
    if (dest.isArray()) return dest;
    auto action = item.getKey("/A");
    if (action.isDictionary() && action.getKey("/S").isName() && action.getKey("/S").getName() == "/GoTo" && action.getKey("/D").isArray()) return action.getKey("/D");
    return Obj::newNull();
}
std::string titleOf(Obj item) {
    auto title = item.getKey("/Title");
    return title.isString() ? title.getUTF8Value() : std::string();
}
// Total rows shown below `parent`, and rewrites each child's /Count so open/closed state is kept.
int recount(Obj parent, int depth) {
    int total = 0;
    std::set<QPDFObjGen> seen;
    for (auto child = parent.getKey("/First"); child.isDictionary(); child = child.getKey("/Next")) {
        if (child.isIndirect() && !seen.insert(child.getObjGen()).second) break;
        ++total;
        if (child.getKey("/First").isDictionary() && depth < maxDepth) {
            auto count = child.getKey("/Count");
            const bool open = count.isInteger() && count.getIntValue() > 0;
            const int below = recount(child, depth + 1);
            child.replaceKey("/Count", Obj::newInteger(open ? below : -below));
            if (open) total += below;
        } else child.removeKey("/Count");
    }
    return total;
}
void refreshCounts(Obj root) {
    const int total = recount(root, 0);
    root.replaceKey("/Count", Obj::newInteger(total));
    if (!root.getKey("/First").isDictionary()) { root.removeKey("/First"); root.removeKey("/Last"); }
}
void detach(Obj item) {
    auto parent = item.getKey("/Parent"), prev = item.getKey("/Prev"), next = item.getKey("/Next");
    if (prev.isDictionary()) { if (next.isDictionary()) prev.replaceKey("/Next", next); else prev.removeKey("/Next"); }
    else if (parent.isDictionary()) { if (next.isDictionary()) parent.replaceKey("/First", next); else parent.removeKey("/First"); }
    if (next.isDictionary()) { if (prev.isDictionary()) next.replaceKey("/Prev", prev); else next.removeKey("/Prev"); }
    else if (parent.isDictionary()) { if (prev.isDictionary()) parent.replaceKey("/Last", prev); else parent.removeKey("/Last"); }
}
Obj rootOutline(QPDF& pdf) {
    auto root = pdf.getRoot();
    auto outlines = root.getKey("/Outlines");
    if (outlines.isDictionary()) return outlines;
    auto created = pdf.makeIndirectObject(Obj::newDictionary());
    created.replaceKey("/Type", Obj::newName("/Outlines"));
    created.replaceKey("/Count", Obj::newInteger(0));
    root.replaceKey("/Outlines", created);
    return created;
}
Node nodeAt(QPDF& pdf, std::uint32_t index) {
    auto nodes = flatten(pdf);
    if (index >= nodes.size()) throw Error(ErrorCode::InvalidSelection, "The bookmark no longer exists.");
    return nodes[index];
}
}
std::vector<Bookmark> bookmarks(QPDF& pdf) {
    auto pages = QPDFPageDocumentHelper(pdf).getAllPages();
    std::vector<Bookmark> out;
    for (const auto& node : flatten(pdf)) {
        Bookmark b; b.index = static_cast<std::uint32_t>(out.size()); b.title = titleOf(node.item); b.level = node.level;
        auto dest = destArray(node.item);
        if (dest.isArray() && dest.getArrayNItems() > 0) {
            auto target = dest.getArrayItem(0);
            if (target.isIndirect())
                for (std::size_t i = 0; i < pages.size(); ++i) if (pages[i].getObjectHandle().getObjGen() == target.getObjGen()) { b.page = static_cast<int>(i); break; }
        }
        out.push_back(std::move(b));
    }
    return out;
}
void addBookmark(QPDF& pdf, const std::string& title, Obj page) {
    if (title.empty() || title.size() > 1000) throw Error(ErrorCode::InvalidSelection, "Enter a bookmark name of up to 1000 bytes.");
    if (flatten(pdf).size() >= maxItems) throw Error(ErrorCode::ResourceLimit, "The bookmark limit has been reached.");
    auto root = rootOutline(pdf);
    auto item = pdf.makeIndirectObject(Obj::newDictionary());
    item.replaceKey("/Title", Obj::newUnicodeString(title));
    item.replaceKey("/Parent", root);
    item.replaceKey("/Dest", Obj::newArray({page, Obj::newName("/Fit")}));
    auto last = root.getKey("/Last");
    if (last.isDictionary()) { last.replaceKey("/Next", item); item.replaceKey("/Prev", last); }
    else root.replaceKey("/First", item);
    root.replaceKey("/Last", item);
    refreshCounts(root);
}
void renameBookmark(QPDF& pdf, std::uint32_t index, const std::string& title) {
    if (title.empty() || title.size() > 1000) throw Error(ErrorCode::InvalidSelection, "Enter a bookmark name of up to 1000 bytes.");
    nodeAt(pdf, index).item.replaceKey("/Title", Obj::newUnicodeString(title));
}
void removeBookmark(QPDF& pdf, std::uint32_t index) {
    auto node = nodeAt(pdf, index);
    detach(node.item);
    refreshCounts(pdf.getRoot().getKey("/Outlines"));
}
void retarget(QPDF& pdf, Obj from, Obj to) {
    for (const auto& node : flatten(pdf)) {
        auto dest = destArray(node.item);
        if (dest.isArray() && dest.getArrayNItems() > 0 && dest.getArrayItem(0).isIndirect() && dest.getArrayItem(0).getObjGen() == from.getObjGen())
            dest.setArrayItem(0, to);
    }
}
void prune(QPDF& pdf) {
    auto root = pdf.getRoot().getKey("/Outlines");
    if (!root.isDictionary()) return;
    std::set<QPDFObjGen> pages;
    for (auto& page : QPDFPageDocumentHelper(pdf).getAllPages()) pages.insert(page.getObjectHandle().getObjGen());
    std::vector<Obj> doomed;
    for (const auto& node : flatten(pdf)) {
        auto dest = destArray(node.item);
        if (dest.isArray() && dest.getArrayNItems() > 0 && dest.getArrayItem(0).isIndirect() && !pages.count(dest.getArrayItem(0).getObjGen())) doomed.push_back(node.item);
    }
    if (doomed.empty()) return;
    for (auto& item : doomed) detach(item);
    refreshCounts(root);
}

namespace {
Obj ocProperties(QPDF& pdf) { return pdf.getRoot().getKey("/OCProperties"); }
Obj groupAt(QPDF& pdf, std::uint32_t index) {
    auto props = ocProperties(pdf); auto groups = props.isDictionary() ? props.getKey("/OCGs") : Obj::newNull();
    if (!groups.isArray() || index >= static_cast<std::uint32_t>(groups.getArrayNItems())) throw Error(ErrorCode::InvalidSelection, "The layer no longer exists.");
    return groups.getArrayItem(static_cast<int>(index));
}
bool listed(Obj array, Obj group) {
    if (!array.isArray()) return false;
    for (int i = 0; i < array.getArrayNItems(); ++i) if (array.getArrayItem(i).isIndirect() && group.isIndirect() && array.getArrayItem(i).getObjGen() == group.getObjGen()) return true;
    return false;
}
// Returns a copy of the array without `group`.
Obj without(Obj array, Obj group) {
    std::vector<Obj> items;
    if (array.isArray()) for (int i = 0; i < array.getArrayNItems(); ++i) {
        auto item = array.getArrayItem(i);
        if (!(item.isIndirect() && group.isIndirect() && item.getObjGen() == group.getObjGen())) items.push_back(item);
    }
    return Obj::newArray(items);
}
Obj with(Obj array, Obj group) {
    std::vector<Obj> items;
    if (array.isArray()) for (int i = 0; i < array.getArrayNItems(); ++i) items.push_back(array.getArrayItem(i));
    items.push_back(group);
    return Obj::newArray(items);
}
bool baseOff(Obj config) {
    auto base = config.isDictionary() ? config.getKey("/BaseState") : Obj::newNull();
    return base.isName() && base.getName() == "/OFF";
}
}
std::vector<Layer> layers(QPDF& pdf) {
    std::vector<Layer> out;
    auto props = ocProperties(pdf);
    if (!props.isDictionary()) return out;
    auto groups = props.getKey("/OCGs"), config = props.getKey("/D");
    if (!groups.isArray()) return out;
    for (int i = 0; i < groups.getArrayNItems() && i < 5000; ++i) {
        auto group = groups.getArrayItem(i);
        Layer layer; layer.index = static_cast<std::uint32_t>(i);
        auto name = group.isDictionary() ? group.getKey("/Name") : Obj::newNull();
        layer.name = name.isString() ? name.getUTF8Value() : "Layer " + std::to_string(i + 1);
        const auto offList = config.isDictionary() ? config.getKey("/OFF") : Obj::newNull(), onList = config.isDictionary() ? config.getKey("/ON") : Obj::newNull();
        layer.visible = baseOff(config) ? listed(onList, group) : !listed(offList, group);
        out.push_back(std::move(layer));
    }
    return out;
}
void setLayerVisible(QPDF& pdf, std::uint32_t index, bool visible) {
    auto group = groupAt(pdf, index);
    auto props = ocProperties(pdf);
    auto config = props.getKey("/D");
    if (!config.isDictionary()) { config = Obj::newDictionary(); props.replaceKey("/D", config); }
    auto on = config.getKey("/ON"), off = config.getKey("/OFF");
    if (visible) {
        config.replaceKey("/OFF", without(off, group));
        if (baseOff(config) && !listed(on, group)) config.replaceKey("/ON", with(on, group));
    } else {
        config.replaceKey("/ON", without(on, group));
        if (!baseOff(config) && !listed(off, group)) config.replaceKey("/OFF", with(off, group));
    }
}
void renameLayer(QPDF& pdf, std::uint32_t index, const std::string& name) {
    if (name.empty() || name.size() > 1000) throw Error(ErrorCode::InvalidSelection, "Enter a layer name of up to 1000 bytes.");
    groupAt(pdf, index).replaceKey("/Name", Obj::newUnicodeString(name));
}
}
