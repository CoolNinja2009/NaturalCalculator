// expr_tree.cpp
#include "expr_tree.h"
#include <cassert>
#include <algorithm>
#include <cstring>
#include <functional>

// ---------------------------------------------------------------- helpers

static char lowerChar(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; }

static std::string lowerName(const std::string& text) {
    std::string out;
    for (char c : text) out += lowerChar(c);
    return out;
}

const char* sciFunctionName(int id) {
    switch (id) {
        case SciSin: return "sin";
        case SciCos: return "cos";
        case SciTan: return "tan";
        case SciAsin: return "asin";
        case SciAcos: return "acos";
        case SciAtan: return "atan";
        case SciSinh: return "sinh";
        case SciCosh: return "cosh";
        case SciTanh: return "tanh";
        case SciLn: return "ln";
        case SciLog: return "log";
        case SciExp: return "exp";
        case SciAbs: return "abs";
    }
    return "?";
}

bool findSciFunction(const std::string& lowerNameIn, int& id) {
    for (int f = 0; f < SciFunctionCount; ++f) {
        if (lowerNameIn == sciFunctionName(f)) { id = f; return true; }
    }
    return false;
}

bool rowIsEmpty(const Row* r) { return r == nullptr || r->items.empty(); }

bool hasEquals(const Row* r) {
    if (!r) return false;
    for (const auto& item : r->items) {
        if (item->type == ItemType::Equals) return true;
    }
    return false;
}

static void attachRow(std::unique_ptr<Row>& slot, Item* owner, Row* ownerParentRow) {
    if (!slot) slot = std::make_unique<Row>();
    slot->owner = owner;
    slot->ownerParentRow = ownerParentRow;
}

// Find the index of `child` (a row belonging to some structural item) within
// its owner item's parent row, i.e. where the *item* sits. Returns -1 if
// child is the root row (no owner).
static int ownerIndexInParentRow(Row* child) {
    if (!child || !child->owner || !child->ownerParentRow) return -1;
    Row* parent = child->ownerParentRow;
    for (size_t i = 0; i < parent->items.size(); ++i) {
        if (parent->items[i].get() == child->owner) return (int)i;
    }
    return -1;
}

// Is `child` the "a" row (numerator/base/inner/radicand) of its owner?
static bool isARow(Row* child) {
    return child && child->owner && child->owner->a.get() == child;
}
static bool isBRow(Row* child) {
    return child && child->owner && child->owner->b.get() == child;
}

// Remove the structural item at `parent->items[ownerIndex]` and splice
// `sourceRow`'s items directly into `parent` in its place, instead of
// discarding them. Used whenever a structural wrapper (Fraction, Power,
// Paren, Sqrt) collapses because one side is empty -- e.g. backspacing an
// empty exponent on "2^" should collapse back to plain "2", not delete
// the base along with the wrapper.
//
// Sets expr.cursor to land in `parent`, either right after the spliced
// content (cursorAfterSpliced == true, the natural backspace resting
// point) or right before it (cursorAfterSpliced == false, the natural
// forward-delete resting point).
static void collapseStructuralItem(Expression& expr, Row* parent, int ownerIndex,
                                    Row* sourceRow, bool cursorAfterSpliced) {
    std::vector<std::unique_ptr<Item>> spliced;
    if (sourceRow) {
        for (auto& it : sourceRow->items) spliced.push_back(std::move(it));
    }
    parent->items.erase(parent->items.begin() + ownerIndex);
    int count = (int)spliced.size();
    for (int i = 0; i < count; ++i) {
        Item* itemPtr = spliced[i].get();
        // Reparent: these items now live directly in `parent`, so any
        // structural children they own must point back to `parent`.
        if (itemPtr->a) itemPtr->a->ownerParentRow = parent;
        if (itemPtr->b) itemPtr->b->ownerParentRow = parent;
        parent->items.insert(parent->items.begin() + ownerIndex + i, std::move(spliced[i]));
    }
    expr.cursor.row = parent;
    expr.cursor.index = cursorAfterSpliced ? (ownerIndex + count) : ownerIndex;
}

// ------------------------------------------------------------- Expression

Expression::Expression() {
    root = std::make_unique<Row>();
    root->owner = nullptr;
    root->ownerParentRow = nullptr;
    cursor.row = root.get();
    cursor.index = 0;
}

static void serializeRow(const Row* r, std::string& out);

static void serializeItem(const Item* it, std::string& out) {
    switch (it->type) {
        case ItemType::Number:
            out += it->numText;
            break;
        case ItemType::Variable:
            out += it->variableName;
            break;
        case ItemType::Operator:
            out += ' ';
            out += it->opChar;
            out += ' ';
            break;
        case ItemType::Equals:
            out += " = ";
            break;
        case ItemType::CloseParen:
            out += ')';
            break;
        case ItemType::Fraction:
            out += '(';
            serializeRow(it->a.get(), out);
            out += ")/(";
            serializeRow(it->b.get(), out);
            out += ')';
            break;
        case ItemType::Paren:
            out += '(';
            serializeRow(it->a.get(), out);
            out += ')';
            break;
        case ItemType::Power:
            out += '(';
            serializeRow(it->a.get(), out);
            out += ")^(";
            serializeRow(it->b.get(), out);
            out += ')';
            break;
        case ItemType::Sqrt:
            out += "sqrt(";
            serializeRow(it->a.get(), out);
            out += ')';
            break;
        case ItemType::Name:
            out += it->nameText;
            break;
        case ItemType::Constant:
            out += (it->constantName == 'p') ? "pi" : "e";
            break;
        case ItemType::Function:
            out += sciFunctionName(it->functionId);
            out += '(';
            serializeRow(it->a.get(), out);
            out += ')';
            break;
    }
}

static void serializeRow(const Row* r, std::string& out) {
    if (!r) return;
    for (auto& it : r->items) serializeItem(it.get(), out);
}

std::string Expression::toPlainString() const {
    std::string out;
    serializeRow(root.get(), out);
    return out;
}

std::string rowRangeToPlainString(const Row* row, int begin, int end) {
    std::string out;
    if (!row) return out;
    begin = std::max(0, begin);
    end = std::min((int)row->items.size(), end);
    for (int i = begin; i < end; ++i) serializeItem(row->items[i].get(), out);
    return out;
}

void deleteRange(Expression& expression, Cursor first, Cursor last) {
    if (!first.row || first.row != last.row) return;
    int begin = std::min(first.index, last.index);
    int end = std::max(first.index, last.index);
    begin = std::max(0, begin);
    end = std::min((int)first.row->items.size(), end);
    if (begin >= end) return;
    first.row->items.erase(first.row->items.begin() + begin,
                           first.row->items.begin() + end);
    expression.cursor.row = first.row;
    expression.cursor.index = begin;
}

static std::unique_ptr<Row> cloneRow(const Row* source, Item* owner, Row* parent) {
    auto copy = std::make_unique<Row>();
    copy->owner = owner;
    copy->ownerParentRow = parent;
    if (!source) return copy;
    for (const auto& sourceItem : source->items) {
        auto item = std::make_unique<Item>(sourceItem->type);
        item->numText = sourceItem->numText;
        item->opChar = sourceItem->opChar;
        item->variableName = sourceItem->variableName;
        item->nameText = sourceItem->nameText;
        item->constantName = sourceItem->constantName;
        item->functionId = sourceItem->functionId;
        Item* itemPtr = item.get();
        if (sourceItem->a) item->a = cloneRow(sourceItem->a.get(), itemPtr, copy.get());
        if (sourceItem->b) item->b = cloneRow(sourceItem->b.get(), itemPtr, copy.get());
        copy->items.push_back(std::move(item));
    }
    return copy;
}

std::unique_ptr<Expression> cloneExpression(const Expression& source) {
    auto copy = std::make_unique<Expression>();
    copy->root = cloneRow(source.root.get(), nullptr, nullptr);
    copy->cursor.row = copy->root.get();
    copy->cursor.index = (int)copy->root->items.size();
    return copy;
}

// ---------------------------------------------------------------- insert

void insertDigit(Expression& expr, char digit) {
    Row* row = expr.cursor.row;
    int idx = expr.cursor.index;

    if (idx > 0 && row->items[idx - 1]->type == ItemType::Number) {
        Item* prev = row->items[idx - 1].get();
        if (digit == '.' && prev->numText.find('.') != std::string::npos)
            return; // only one decimal point per number
        prev->numText += digit;
        return;
    }

    auto item = std::make_unique<Item>(ItemType::Number);
    item->numText = std::string(1, digit);
    row->items.insert(row->items.begin() + idx, std::move(item));
    expr.cursor.index = idx + 1;
}

void insertVariable(Expression& expr, char name) {
    if (name != 'x' && name != 'y') return;
    Row* row = expr.cursor.row;
    int idx = expr.cursor.index;
    auto item = std::make_unique<Item>(ItemType::Variable);
    item->variableName = name;
    row->items.insert(row->items.begin() + idx, std::move(item));
    expr.cursor.index = idx + 1;
}

void insertOperator(Expression& expr, char op) {
    if (op != '-' && isBRow(expr.cursor.row) && expr.cursor.row->owner &&
        expr.cursor.row->owner->type == ItemType::Power &&
        expr.cursor.index == (int)expr.cursor.row->items.size()) {
        moveRight(expr);
    }
    Row* row = expr.cursor.row;
    int idx = expr.cursor.index;
    auto item = std::make_unique<Item>(ItemType::Operator);
    item->opChar = op;
    row->items.insert(row->items.begin() + idx, std::move(item));
    expr.cursor.index = idx + 1;
}

void insertEquals(Expression& expr) {
    if (isBRow(expr.cursor.row) && expr.cursor.row->owner &&
        expr.cursor.row->owner->type == ItemType::Power &&
        expr.cursor.index == (int)expr.cursor.row->items.size()) {
        moveRight(expr);
    }
    Row* row = expr.cursor.row;
    int idx = expr.cursor.index;
    auto item = std::make_unique<Item>(ItemType::Equals);
    row->items.insert(row->items.begin() + idx, std::move(item));
    expr.cursor.index = idx + 1;
}

// Consume the atom immediately to the left of the cursor (a whole Number
// item, or a whole structural item) and return it, adjusting cursor.index
// and the row in the process. Returns nullptr (and leaves cursor alone) if
// there is nothing consumable (start of row, or previous is an Operator).
static std::unique_ptr<Item> consumeLeftAtom(Expression& expr) {
    Row* row = expr.cursor.row;
    int idx = expr.cursor.index;
    if (idx == 0) return nullptr;
    Item* prev = row->items[idx - 1].get();
    if (prev->type == ItemType::Operator) return nullptr;

    std::unique_ptr<Item> taken = std::move(row->items[idx - 1]);
    row->items.erase(row->items.begin() + (idx - 1));
    expr.cursor.index = idx - 1;
    return taken;
}

// When an existing structural item (Paren/Sqrt/Fraction/Power) is moved
// into a different row -- e.g. consumeLeftAtom() pulls it out to become
// the numerator of a brand-new Fraction -- the item's own child rows
// still think they live in the row they were *originally* created in
// (Row::ownerParentRow is set once, at creation, and otherwise never
// updated). Left uncorrected, that stale pointer makes
// ownerIndexInParentRow() search the wrong row for the item, silently
// breaking arrow-key navigation and backspace/delete for anything nested
// inside the moved item (e.g. typing "(2+3)" and then pressing '/' or '^'
// to wrap it). Call this immediately after re-homing such an item.
static void reparentItemChildren(Item* item, Row* newParentRow) {
    if (!item) return;
    if (item->a) item->a->ownerParentRow = newParentRow;
    if (item->b) item->b->ownerParentRow = newParentRow;
}

void insertFraction(Expression& expr) {
    Row* row = expr.cursor.row;
    int insertPos = expr.cursor.index;

    auto fracItem = std::make_unique<Item>(ItemType::Fraction);
    Item* fracPtr = fracItem.get();

    std::unique_ptr<Item> consumed = consumeLeftAtom(expr);
    insertPos = expr.cursor.index; // consumeLeftAtom may have shifted it

    attachRow(fracItem->a, fracPtr, row);
    attachRow(fracItem->b, fracPtr, row);
    if (consumed) {
        reparentItemChildren(consumed.get(), fracItem->a.get());
        fracItem->a->items.push_back(std::move(consumed));
    }

    row->items.insert(row->items.begin() + insertPos, std::move(fracItem));

    // cursor -> start of denominator
    expr.cursor.row = fracPtr->b.get();
    expr.cursor.index = 0;
}

void insertPower(Expression& expr) {
    if (isBRow(expr.cursor.row) && expr.cursor.row->owner &&
        expr.cursor.row->owner->type == ItemType::Power &&
        expr.cursor.index == (int)expr.cursor.row->items.size()) {
        moveRight(expr);
    }
    Row* row = expr.cursor.row;
    int insertPos = expr.cursor.index;

    auto powItem = std::make_unique<Item>(ItemType::Power);
    Item* powPtr = powItem.get();

    std::unique_ptr<Item> consumed = consumeLeftAtom(expr);
    insertPos = expr.cursor.index;

    attachRow(powItem->a, powPtr, row);
    attachRow(powItem->b, powPtr, row);
    if (consumed) {
        reparentItemChildren(consumed.get(), powItem->a.get());
        powItem->a->items.push_back(std::move(consumed));
    }

    row->items.insert(row->items.begin() + insertPos, std::move(powItem));

    // cursor -> start of exponent
    expr.cursor.row = powPtr->b.get();
    expr.cursor.index = 0;
}

void insertSqrt(Expression& expr) {
    Row* row = expr.cursor.row;
    int idx = expr.cursor.index;

    auto sqItem = std::make_unique<Item>(ItemType::Sqrt);
    Item* sqPtr = sqItem.get();
    attachRow(sqItem->a, sqPtr, row);

    row->items.insert(row->items.begin() + idx, std::move(sqItem));
    expr.cursor.row = sqPtr->a.get();
    expr.cursor.index = 0;
}

void insertOpenParen(Expression& expr) {
    Row* row = expr.cursor.row;
    int idx = expr.cursor.index;

    auto pItem = std::make_unique<Item>(ItemType::Paren);
    Item* pPtr = pItem.get();
    attachRow(pItem->a, pPtr, row);

    row->items.insert(row->items.begin() + idx, std::move(pItem));
    expr.cursor.row = pPtr->a.get();
    expr.cursor.index = 0;
}

void insertCloseParen(Expression& expr) {
    // ')' is structural, not a typed glyph (see header comment): if the
    // cursor sits anywhere inside an unclosed Paren's inner row, step out
    // to just after that Paren -- regardless of whether the cursor is at
    // the row's end, since any content still to the right stays exactly
    // where it is (inside the paren) either way. This matches how natural
    // display calculators (Casio fx-991ES) resolve ')'.
    //
    // If the cursor is NOT inside an open paren, do nothing. Inserting a
    // bare ')' character here would draw as a small flat glyph instead of
    // the tall stretched bracket a real Paren renders, which is exactly
    // the "mismatched bracket" look this avoids.
    //
    // Function and Sqrt argument rows count too: their ')' is part of the
    // call itself ("sin(30)"), so a ')' typed or pasted while the cursor
    // is inside the argument steps out of the call rather than littering
    // the argument row with an unmatched glyph.
    Row* insertionRow = expr.cursor.row;
    int insertionIndex = expr.cursor.index;
    Row* row = insertionRow;
    while (row && row->owner) {
        int k = ownerIndexInParentRow(row);
        if (k < 0) break;
        if (row->owner->a.get() == row &&
            (row->owner->type == ItemType::Paren ||
             row->owner->type == ItemType::Function ||
             row->owner->type == ItemType::Sqrt)) {
            expr.cursor.row = row->ownerParentRow;
            expr.cursor.index = k + 1;
            return;
        }
        row = row->ownerParentRow;
    }
    auto close = std::make_unique<Item>(ItemType::CloseParen);
    insertionRow->items.insert(insertionRow->items.begin() + insertionIndex, std::move(close));
    expr.cursor.row = insertionRow;
    expr.cursor.index = insertionIndex + 1;
}

// ---------------------------------------------------------------- motion

void moveLeft(Expression& expr) {
    Row* row = expr.cursor.row;
    int idx = expr.cursor.index;

    if (idx > 0) {
        Item* prev = row->items[idx - 1].get();
        switch (prev->type) {
            case ItemType::Number:
            case ItemType::Variable:
            case ItemType::Name:
            case ItemType::Constant:
            case ItemType::Equals:
            case ItemType::CloseParen:
            case ItemType::Operator:
                expr.cursor.index = idx - 1;
                return;
            case ItemType::Fraction:
            case ItemType::Power:
                // Entering a closed fraction/power from the right lands
                // in its rightmost row -- the denominator/exponent --
                // not the numerator/base. This mirrors how the caret
                // exited it in the first place when it was built left to
                // right, and matches natural-display calculators (Casio
                // fx-991ES): arrowing left over "2/3" steps into "3".
                expr.cursor.row = prev->b.get();
                expr.cursor.index = (int)prev->b->items.size();
                return;
            case ItemType::Paren:
            case ItemType::Sqrt:
            case ItemType::Function:
                // These have only one child row, so entering from either
                // side lands in the same place: its end.
                expr.cursor.row = prev->a.get();
                expr.cursor.index = (int)prev->a->items.size();
                return;
        }
    }

    // idx == 0: step out of the current structure, if any.
    if (row->owner) {
        // Leaving the start of a denominator/exponent steps sideways into
        // the end of its sibling numerator/base, instead of exiting the
        // whole structure -- otherwise the numerator would be completely
        // unreachable by arrow keys once you'd arrowed into the
        // denominator.
        if (isBRow(row) &&
            (row->owner->type == ItemType::Fraction || row->owner->type == ItemType::Power)) {
            Row* a = row->owner->a.get();
            expr.cursor.row = a;
            expr.cursor.index = (int)a->items.size();
            return;
        }
        int k = ownerIndexInParentRow(row);
        if (k >= 0) {
            expr.cursor.row = row->ownerParentRow;
            expr.cursor.index = k; // land just before the owning item
        }
    }
    // else: already at the very start of the whole expression; no-op.
}

void moveRight(Expression& expr) {
    Row* row = expr.cursor.row;
    int idx = expr.cursor.index;

    if (idx < (int)row->items.size()) {
        Item* next = row->items[idx].get();
        switch (next->type) {
            case ItemType::Number:
            case ItemType::Variable:
            case ItemType::Name:
            case ItemType::Constant:
            case ItemType::Equals:
            case ItemType::CloseParen:
            case ItemType::Operator:
                expr.cursor.index = idx + 1;
                return;
            case ItemType::Fraction:
            case ItemType::Power:
            case ItemType::Paren:
            case ItemType::Sqrt:
            case ItemType::Function:
                expr.cursor.row = next->a.get();
                expr.cursor.index = 0;
                return;
        }
    }

    // idx == end of row: step out of the current structure, if any.
    if (row->owner) {
        // Leaving the end of a numerator/base steps sideways into the
        // start of its sibling denominator/exponent, instead of exiting
        // the whole structure -- otherwise the denominator would be
        // completely unreachable by arrow keys.
        if (isARow(row) &&
            (row->owner->type == ItemType::Fraction || row->owner->type == ItemType::Power)) {
            Row* b = row->owner->b.get();
            expr.cursor.row = b;
            expr.cursor.index = 0;
            return;
        }
        int k = ownerIndexInParentRow(row);
        if (k >= 0) {
            expr.cursor.row = row->ownerParentRow;
            expr.cursor.index = k + 1; // land just after the owning item
        }
    }
    // else: already at the very end of the whole expression; no-op.
}

// Returns true if the cursor actually moved (i.e. it was inside a
// denominator/exponent and stepped up into the numerator/base). Callers
// (e.g. the Up arrow key handler) use this to fall back to other
// behavior -- like recalling history -- only when there is genuinely
// nowhere to navigate to inside the current expression.
bool moveUp(Expression& expr) {
    Row* row = expr.cursor.row;
    if (isBRow(row)) { // in denominator/exponent -> go to numerator/base
        Item* owner = row->owner;
        Row* target = owner->a.get();
        expr.cursor.row = target;
        if (expr.cursor.index > (int)target->items.size())
            expr.cursor.index = (int)target->items.size();
        return true;
    }
    // In a-row, or in a single-row structure (Paren/Sqrt), or in root:
    // no vertical sibling to move to (simplification).
    return false;
}

// Mirror of moveUp: returns true if the cursor moved.
bool moveDown(Expression& expr) {
    Row* row = expr.cursor.row;
    if (isARow(row) && row->owner &&
        (row->owner->type == ItemType::Fraction || row->owner->type == ItemType::Power)) {
        Item* owner = row->owner;
        Row* target = owner->b.get();
        expr.cursor.row = target;
        if (expr.cursor.index > (int)target->items.size())
            expr.cursor.index = (int)target->items.size();
        return true;
    }
    return false;
}

// -------------------------------------------------------------- deletion

void backspace(Expression& expr) {
    Row* row = expr.cursor.row;
    int idx = expr.cursor.index;

    if (idx == 0) {
        if (!row->owner) return; // start of whole expression

        Item* ownerItem = row->owner;
        Row* parent = row->ownerParentRow;
        int ownerIndex = ownerIndexInParentRow(row);
        if (ownerIndex < 0) return;

        if (isBRow(row)) {
            if (rowIsEmpty(row)) {
                // Empty denominator/exponent: collapse the wrapper and
                // splice the numerator/base back into the parent row so
                // its content survives (e.g. "2^" -> backspace -> "2",
                // not "" ). Two backspaces now correctly remove first the
                // exponent's digit, then the "^" box itself.
                collapseStructuralItem(expr, parent, ownerIndex, ownerItem->a.get(), true);
                return;
            }
            // Backspace at the very start of a denominator/exponent steps
            // back into the end of the numerator/base, matching natural
            // calculators (no data is lost).
            Row* a = ownerItem->a.get();
            expr.cursor.row = a;
            expr.cursor.index = (int)a->items.size();
            return;
        }

        // row is the item's primary row (numerator/base for Fraction/
        // Power, or the sole inner/radicand row for Paren/Sqrt), and the
        // cursor sits at its very start.
        bool siblingHasContent = (ownerItem->type == ItemType::Fraction ||
                                   ownerItem->type == ItemType::Power) &&
                                  !rowIsEmpty(ownerItem->b.get());

        if (rowIsEmpty(row)) {
            if (!siblingHasContent) {
                // Nothing anywhere in the structure: it was just opened
                // (or is otherwise fully empty) -- safe to remove outright.
                parent->items.erase(parent->items.begin() + ownerIndex);
                expr.cursor.row = parent;
                expr.cursor.index = ownerIndex;
            } else {
                // The primary row is empty but the other side (e.g. an
                // already-filled denominator/exponent reached by moving
                // up into an empty numerator/base) has content: collapse
                // the wrapper and keep that content instead of losing it.
                collapseStructuralItem(expr, parent, ownerIndex, ownerItem->b.get(), true);
            }
            return;
        }

        // The primary row has real content and the cursor is at its left
        // boundary: step out of the structure without destroying
        // anything, just like moveLeft does at this position.
        expr.cursor.row = parent;
        expr.cursor.index = ownerIndex;
        return;
    }

    Item* target = row->items[idx - 1].get();
    if (target->type == ItemType::Number) {
        if (target->numText.size() > 1) {
            target->numText.pop_back();
        } else {
            row->items.erase(row->items.begin() + (idx - 1));
            expr.cursor.index = idx - 1;
        }
        return;
    }
    if (target->type == ItemType::Name) {
        // Deleting into a word erases its last letter, like a number run.
        if (target->nameText.size() > 1) {
            target->nameText.pop_back();
        } else {
            row->items.erase(row->items.begin() + (idx - 1));
            expr.cursor.index = idx - 1;
        }
        return;
    }
    if (target->type == ItemType::Variable) {
        row->items.erase(row->items.begin() + (idx - 1));
        expr.cursor.index = idx - 1;
        return;
    }
    if (target->type == ItemType::Operator || target->type == ItemType::CloseParen ||
        target->type == ItemType::Equals) {
        row->items.erase(row->items.begin() + (idx - 1));
        expr.cursor.index = idx - 1;
        return;
    }

    // Structural neighbor.
    bool aEmpty = rowIsEmpty(target->a.get());
    bool bEmpty = (target->type == ItemType::Fraction || target->type == ItemType::Power)
                      ? rowIsEmpty(target->b.get())
                      : true;
    if (aEmpty && bEmpty) {
        row->items.erase(row->items.begin() + (idx - 1));
        expr.cursor.index = idx - 1;
        return;
    }

    // Non-empty structure: first backspace "enters" it (lands at the end
    // of its rightmost row) rather than deleting its contents outright.
    Row* enter = (target->type == ItemType::Fraction || target->type == ItemType::Power)
                     ? target->b.get()
                     : target->a.get();
    expr.cursor.row = enter;
    expr.cursor.index = (int)enter->items.size();
}

void doDelete(Expression& expr) {
    Row* row = expr.cursor.row;
    int idx = expr.cursor.index;

    if (idx == (int)row->items.size()) {
        if (!row->owner) return; // end of whole expression

        Item* ownerItem = row->owner;
        Row* parent = row->ownerParentRow;
        int ownerIndex = ownerIndexInParentRow(row);
        if (ownerIndex < 0) return;

        if (isBRow(row)) {
            if (rowIsEmpty(row)) {
                // Empty denominator/exponent, cursor at its end: collapse
                // the wrapper and keep the numerator/base's content
                // instead of discarding it.
                collapseStructuralItem(expr, parent, ownerIndex, ownerItem->a.get(), true);
                return;
            }
            // Non-empty denominator/exponent, cursor at its end: step out
            // of the structure (mirrors moveRight) rather than deleting
            // real content.
            expr.cursor.row = parent;
            expr.cursor.index = ownerIndex + 1;
            return;
        }

        if (isARow(row) &&
            (ownerItem->type == ItemType::Fraction || ownerItem->type == ItemType::Power)) {
            // Delete at the very end of numerator/base steps forward into
            // the start of denominator/exponent.
            Row* b = ownerItem->b.get();
            expr.cursor.row = b;
            expr.cursor.index = 0;
            return;
        }

        // row is the sole inner/radicand row of a Paren/Sqrt, cursor at
        // its end.
        if (rowIsEmpty(row)) {
            parent->items.erase(parent->items.begin() + ownerIndex);
            expr.cursor.row = parent;
            expr.cursor.index = ownerIndex;
        } else {
            // Real content inside: step out without destroying it.
            expr.cursor.row = parent;
            expr.cursor.index = ownerIndex + 1;
        }
        return;
    }

    Item* target = row->items[idx].get();
    if (target->type == ItemType::Number) {
        if (target->numText.size() > 1) {
            target->numText.erase(target->numText.begin());
        } else {
            row->items.erase(row->items.begin() + idx);
        }
        return;
    }
    if (target->type == ItemType::Name) {
        if (target->nameText.size() > 1) {
            target->nameText.erase(target->nameText.begin());
        } else {
            row->items.erase(row->items.begin() + idx);
        }
        return;
    }
    if (target->type == ItemType::Variable) {
        row->items.erase(row->items.begin() + idx);
        return;
    }
    if (target->type == ItemType::Operator || target->type == ItemType::CloseParen ||
        target->type == ItemType::Equals) {
        row->items.erase(row->items.begin() + idx);
        return;
    }

    bool aEmpty = rowIsEmpty(target->a.get());
    bool bEmpty = (target->type == ItemType::Fraction || target->type == ItemType::Power)
                      ? rowIsEmpty(target->b.get())
                      : true;
    if (aEmpty && bEmpty) {
        row->items.erase(row->items.begin() + idx);
        return;
    }

    // Enter the structure from its primary (leftmost) row instead of
    // deleting its contents outright.
    expr.cursor.row = target->a.get();
    expr.cursor.index = 0;
}

// ------------------------------------------------------- scientific items

void insertFunction(Expression& expr, int functionId) {
    if (functionId < 0 || functionId >= SciFunctionCount) return;
    Row* row = expr.cursor.row;
    int idx = expr.cursor.index;

    auto item = std::make_unique<Item>(ItemType::Function);
    item->functionId = functionId;
    Item* ptr = item.get();
    attachRow(item->a, ptr, row);

    row->items.insert(row->items.begin() + idx, std::move(item));
    expr.cursor.row = ptr->a.get();
    expr.cursor.index = 0;
}

void insertConstant(Expression& expr, char which) {
    if (which != 'p' && which != 'e') return;
    Row* row = expr.cursor.row;
    int idx = expr.cursor.index;
    auto item = std::make_unique<Item>(ItemType::Constant);
    item->constantName = which;
    row->items.insert(row->items.begin() + idx, std::move(item));
    expr.cursor.index = idx + 1;
}

void insertNameLetter(Expression& expr, char letter) {
    letter = lowerChar(letter);
    if (letter < 'a' || letter > 'z') return;
    Row* row = expr.cursor.row;
    int idx = expr.cursor.index;

    if (idx > 0 && row->items[idx - 1]->type == ItemType::Name) {
        row->items[idx - 1]->nameText += letter;
        return; // cursor stays right after the word
    }
    auto item = std::make_unique<Item>(ItemType::Name);
    item->nameText = std::string(1, letter);
    row->items.insert(row->items.begin() + idx, std::move(item));
    expr.cursor.index = idx + 1;
}

// Resolve one row's Name items. Returns true if any conversion happened.
static bool normalizeRowNames(Expression& expr, Row* row) {
    if (!row) return false;
    bool changed = false;

    // Merge adjacent Name items ("s" + "in" -> "sin").
    for (size_t i = 0; i + 1 < row->items.size();) {
        if (row->items[i]->type == ItemType::Name &&
            row->items[i + 1]->type == ItemType::Name) {
            row->items[i]->nameText += row->items[i + 1]->nameText;
            row->items.erase(row->items.begin() + i + 1);
            if (expr.cursor.row == row && expr.cursor.index > (int)i)
                expr.cursor.index = std::max((int)i, expr.cursor.index - 1);
            changed = true;
        } else {
            ++i;
        }
    }

    for (size_t i = 0; i < row->items.size(); ++i) {
        Item* it = row->items[i].get();
        if (it->type != ItemType::Name) continue;
        const std::string lower = lowerName(it->nameText);

        // "sin(" (Name followed directly by a Paren) becomes a real call:
        // the paren's inner row is adopted as the argument.
        Item* next = (i + 1 < row->items.size()) ? row->items[i + 1].get() : nullptr;
        if (next && next->type == ItemType::Paren) {
            int functionId = -1;
            bool isSqrt = (lower == "sqrt");
            if (isSqrt || findSciFunction(lower, functionId)) {
                auto replacement = std::make_unique<Item>(
                    isSqrt ? ItemType::Sqrt : ItemType::Function);
                if (!isSqrt) replacement->functionId = functionId;
                Item* repPtr = replacement.get();
                repPtr->a = std::move(next->a);
                repPtr->a->owner = repPtr; // ownerParentRow is unchanged
                row->items[i] = std::move(replacement);
                row->items.erase(row->items.begin() + i + 1);
                if (expr.cursor.row == row && expr.cursor.index > (int)i + 1)
                    expr.cursor.index--;
                changed = true;
                continue;
            }
        }
        // Standalone words resolve to constants / variables.
        if (lower == "pi") {
            it->type = ItemType::Constant; it->constantName = 'p'; it->nameText.clear();
            changed = true;
        } else if (lower == "e") {
            it->type = ItemType::Constant; it->constantName = 'e'; it->nameText.clear();
            changed = true;
        } else if (lower == "x") {
            it->type = ItemType::Variable; it->variableName = 'x'; it->nameText.clear();
            changed = true;
        } else if (lower == "y") {
            it->type = ItemType::Variable; it->variableName = 'y'; it->nameText.clear();
            changed = true;
        }
    }
    return changed;
}

void normalizeNames(Expression& expr) {
    // Depth-first over every row (children first: conversions at this level
    // move rows around, but normalized subtrees are untouched by that).
    std::function<void(Row*)> walk = [&](Row* row) {
        if (!row) return;
        for (auto& it : row->items) {
            if (it->a) walk(it->a.get());
            if (it->b) walk(it->b.get());
        }
        normalizeRowNames(expr, row);
    };
    walk(expr.root.get());
    if (expr.cursor.row)
        expr.cursor.index = std::clamp(expr.cursor.index, 0, (int)expr.cursor.row->items.size());
}

// Longest function/constant name matching at run[k..]; returns match length
// (0 = none). sqrt is matched here too even though it builds a Sqrt item.
static size_t matchNameToken(const std::string& run, size_t k, int& functionId, bool& isSqrt,
                             bool& isPi) {
    functionId = -1; isSqrt = false; isPi = false;
    size_t best = 0;
    for (int f = 0; f < SciFunctionCount; ++f) {
        const char* name = sciFunctionName(f);
        size_t len = std::char_traits<char>::length(name);
        if (len > best && run.size() - k >= len && run.compare(k, len, name) == 0) {
            best = len; functionId = f; isSqrt = false; isPi = false;
        }
    }
    if (4 > best && run.size() - k >= 4 && run.compare(k, 4, "sqrt") == 0) {
        best = 4; functionId = -1; isSqrt = true; isPi = false;
    }
    if (2 > best && run.size() - k >= 2 && run.compare(k, 2, "pi") == 0) {
        best = 2; functionId = -1; isSqrt = false; isPi = true;
    }
    return best;
}

void insertFromText(Expression& expr, const std::string& text) {
    size_t i = 0;
    while (i < text.size()) {
        char c = text[i];
        if ((c >= '0' && c <= '9') || c == '.') { insertDigit(expr, c); ++i; continue; }
        if (c == '+' || c == '-' || c == '*' || c == '!') { insertOperator(expr, c); ++i; continue; }
        if (c == '/') { insertFraction(expr); ++i; continue; }
        if (c == '=') { insertEquals(expr); ++i; continue; }
        if (c == '(') { insertOpenParen(expr); ++i; continue; }
        if (c == ')') { insertCloseParen(expr); ++i; continue; }
        if (c == '^') { insertPower(expr); ++i; continue; }

        if (lowerChar(c) >= 'a' && lowerChar(c) <= 'z') {
            size_t j = i;
            while (j < text.size() &&
                   ((lowerChar(text[j]) >= 'a' && lowerChar(text[j]) <= 'z')))
                ++j;
            std::string run = lowerName(text.substr(i, j - i));
            bool functionOpen = false;
            size_t k = 0;
            while (k < run.size()) {
                int functionId = -1;
                bool isSqrt = false, isPi = false;
                size_t len = matchNameToken(run, k, functionId, isSqrt, isPi);
                if (len > 0) {
                    if (isPi) {
                        insertConstant(expr, 'p');
                    } else if (isSqrt) {
                        insertSqrt(expr);
                        functionOpen = true; // the '(' in the text is consumed
                    } else {
                        insertFunction(expr, functionId);
                        functionOpen = true;
                    }
                    k += len;
                    continue;
                }
                char letter = run[k];
                if (letter == 'x' || letter == 'y') {
                    insertVariable(expr, letter);
                } else if (letter == 'e') {
                    insertConstant(expr, 'e');
                }
                // anything else in an unrecognized word is dropped
                ++k;
            }
            // The '(' right after a word that opened a call belongs to it.
            if (functionOpen && j < text.size() && text[j] == '(')
                ++j;
            i = j;
            continue;
        }
        ++i; // unknown characters are dropped, as before
    }
    normalizeNames(expr);
}
