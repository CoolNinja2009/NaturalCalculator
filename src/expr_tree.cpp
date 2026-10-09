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
        case SciSec: return "sec";
        case SciCsc: return "csc";
        case SciCot: return "cot";
        case SciAsec: return "asec";
        case SciAcsc: return "acsc";
        case SciAcot: return "acot";
        case SciSinh: return "sinh";
        case SciCosh: return "cosh";
        case SciTanh: return "tanh";
        case SciAsinh: return "asinh";
        case SciAcosh: return "acosh";
        case SciAtanh: return "atanh";
        case SciSech: return "sech";
        case SciCsch: return "csch";
        case SciCoth: return "coth";
        case SciAsech: return "asech";
        case SciAcsch: return "acsch";
        case SciAcoth: return "acoth";
        case SciLn: return "ln";
        case SciLog: return "log";
        case SciLog2: return "log2";
        case SciExp: return "exp";
        case SciExpm1: return "expm1";
        case SciLog1p: return "log1p";
        case SciCbrt: return "cbrt";
        case SciAbs: return "abs";
        case SciFloor: return "floor";
        case SciCeil: return "ceil";
        case SciRound: return "round";
        case SciTrunc: return "trunc";
        case SciSgn: return "sgn";
        case SciGamma: return "gamma";
        case SciLgamma: return "lgamma";
        case SciErf: return "erf";
        case SciErfc: return "erfc";
        case SciFact: return "fact";
        case SciDeg: return "deg";
        case SciRad: return "rad";
        // Calculus
        case SciIntegrate: return "integrate";
        case SciDiff: return "diff";
        case SciLimit: return "limit";
        case SciSum: return "sum";
        case SciProduct: return "product";
        // Matrix & Vector
        case SciDet: return "det";
        case SciInv: return "inv";
        case SciTranspose: return "transpose";
        case SciTrace: return "trace";
        case SciRank: return "rank";
        case SciRref: return "rref";
        case SciDot: return "dot";
        case SciCross: return "cross";
        case SciNorm: return "norm";
        case SciEye: return "eye";
        case SciZeros: return "zeros";
        case SciOnes: return "ones";
        // Statistics
        case SciMean: return "mean";
        case SciMedian: return "median";
        case SciStddev: return "stddev";
        case SciVar: return "var";
        case SciMin: return "min";
        case SciMax: return "max";
        // Number Theory
        case SciGcd: return "gcd";
        case SciLcm: return "lcm";
        case SciIsPrime: return "isprime";
        case SciNcrFn: return "ncr";
        case SciNprFn: return "npr";
        // Advanced
        case SciBeta: return "beta";
        case SciBesselJ0: return "besselj0";
        case SciBesselJ1: return "besselj1";
        case SciBesselY0: return "bessely0";
        case SciBesselY1: return "bessely1";
        case SciSinc: return "sinc";
        case SciLambertW: return "lambertw";
        case SciHypot: return "hypot";
        case SciAtan2: return "atan2";
        case SciZeta: return "zeta";
        case SciClamp: return "clamp";
        case SciLerp: return "lerp";
    }
    return "?";
}

bool findSciFunction(const std::string& lowerNameIn, int& id) {
    for (int f = 0; f < SciFunctionCount; ++f) {
        if (lowerNameIn == sciFunctionName(f)) { id = f; return true; }
    }
    // Aliases
    if (lowerNameIn == "arcsin") { id = SciAsin; return true; }
    if (lowerNameIn == "arccos") { id = SciAcos; return true; }
    if (lowerNameIn == "arctan") { id = SciAtan; return true; }
    if (lowerNameIn == "cosec") { id = SciCsc; return true; }
    if (lowerNameIn == "arcsec") { id = SciAsec; return true; }
    if (lowerNameIn == "arccsc" || lowerNameIn == "arccosec") { id = SciAcsc; return true; }
    if (lowerNameIn == "arccot") { id = SciAcot; return true; }
    if (lowerNameIn == "arcsinh") { id = SciAsinh; return true; }
    if (lowerNameIn == "arccosh") { id = SciAcosh; return true; }
    if (lowerNameIn == "arctanh") { id = SciAtanh; return true; }
    if (lowerNameIn == "arcsech") { id = SciAsech; return true; }
    if (lowerNameIn == "arccsch") { id = SciAcsch; return true; }
    if (lowerNameIn == "arccoth") { id = SciAcoth; return true; }
    if (lowerNameIn == "log10") { id = SciLog; return true; }
    if (lowerNameIn == "ceiling") { id = SciCeil; return true; }
    if (lowerNameIn == "sign" || lowerNameIn == "signum") { id = SciSgn; return true; }
    if (lowerNameIn == "lngamma") { id = SciLgamma; return true; }
    if (lowerNameIn == "factorial") { id = SciFact; return true; }
    if (lowerNameIn == "degrees" || lowerNameIn == "todeg") { id = SciDeg; return true; }
    if (lowerNameIn == "radians" || lowerNameIn == "torad") { id = SciRad; return true; }
    // Calculus aliases
    if (lowerNameIn == "int" || lowerNameIn == "integral") { id = SciIntegrate; return true; }
    if (lowerNameIn == "derivative") { id = SciDiff; return true; }
    if (lowerNameIn == "lim") { id = SciLimit; return true; }
    if (lowerNameIn == "sigma") { id = SciSum; return true; }
    if (lowerNameIn == "prod") { id = SciProduct; return true; }
    // Matrix aliases
    if (lowerNameIn == "inverse") { id = SciInv; return true; }
    if (lowerNameIn == "trans") { id = SciTranspose; return true; }
    if (lowerNameIn == "tr") { id = SciTrace; return true; }
    if (lowerNameIn == "mag" || lowerNameIn == "magnitude") { id = SciNorm; return true; }
    if (lowerNameIn == "identity") { id = SciEye; return true; }
    // Stats aliases
    if (lowerNameIn == "avg" || lowerNameIn == "average") { id = SciMean; return true; }
    if (lowerNameIn == "stdev") { id = SciStddev; return true; }
    if (lowerNameIn == "variance") { id = SciVar; return true; }
    // Number Theory aliases
    if (lowerNameIn == "hcf") { id = SciGcd; return true; }
    if (lowerNameIn == "prime") { id = SciIsPrime; return true; }
    if (lowerNameIn == "comb" || lowerNameIn == "combinations") { id = SciNcrFn; return true; }
    if (lowerNameIn == "perm" || lowerNameIn == "permutations") { id = SciNprFn; return true; }
    // Advanced aliases
    if (lowerNameIn == "j0") { id = SciBesselJ0; return true; }
    if (lowerNameIn == "j1") { id = SciBesselJ1; return true; }
    if (lowerNameIn == "y0") { id = SciBesselY0; return true; }
    if (lowerNameIn == "y1") { id = SciBesselY1; return true; }
    if (lowerNameIn == "lambert") { id = SciLambertW; return true; }
    return false;
}

bool rowIsEmpty(const Row* r) { return r == nullptr || r->items.empty(); }

static bool isTwoRowStructure(ItemType t) {
    return t == ItemType::Fraction || t == ItemType::Power ||
           t == ItemType::Permutation || t == ItemType::Combination ||
           t == ItemType::Derivative;
}

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
static bool isCRow(Row* child) {
    return child && child->owner && child->owner->c.get() == child;
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
        if (itemPtr->c) itemPtr->c->ownerParentRow = parent;
        if (itemPtr->d) itemPtr->d->ownerParentRow = parent;
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

static std::string toSuperscriptString(const std::string& text) {
    std::string out;
    for (char c : text) {
        switch (c) {
            case '0': out += "\xE2\x81\xB0"; break;
            case '1': out += "\xC2\xB9"; break;
            case '2': out += "\xC2\xB2"; break;
            case '3': out += "\xC2\xB3"; break;
            case '4': out += "\xE2\x81\xB4"; break;
            case '5': out += "\xE2\x81\xB5"; break;
            case '6': out += "\xE2\x81\xB6"; break;
            case '7': out += "\xE2\x81\xB7"; break;
            case '8': out += "\xE2\x81\xB8"; break;
            case '9': out += "\xE2\x81\xB9"; break;
            default: out += c; break;
        }
    }
    return out;
}

static std::string toSubscriptString(const std::string& text) {
    std::string out;
    for (char c : text) {
        if (c >= '0' && c <= '9') {
            out += "\xE2\x82";
            out += (char)(0x80 + (c - '0'));
        } else {
            out += c;
        }
    }
    return out;
}

static bool isSimpleNumberRow(const Row* r, std::string& num) {
    if (!r || r->items.size() != 1 || r->items[0]->type != ItemType::Number) return false;
    num = r->items[0]->numText;
    for (char c : num) if (c < '0' || c > '9') return false;
    return !num.empty();
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
            if (it->opChar == 'U') out += " U ";
            else if (it->opChar == 'I') out += " \xE2\x88\xA9 ";
            else if (it->opChar == 'D') out += " \xCE\x94 ";
            else {
                out += ' ';
                out += it->opChar;
                out += ' ';
            }
            break;
        case ItemType::Equals:
            out += " = ";
            break;
        case ItemType::CloseParen:
            out += it->isBrace ? '}' : (it->isBracket ? ']' : ')');
            break;
        case ItemType::Fraction:
            out += '(';
            serializeRow(it->a.get(), out);
            out += ")/(";
            serializeRow(it->b.get(), out);
            out += ')';
            break;
        case ItemType::Paren:
            out += it->isBrace ? '{' : (it->isBracket ? '[' : '(');
            serializeRow(it->a.get(), out);
            out += it->isBrace ? '}' : (it->isBracket ? ']' : ')');
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
            if (it->constantName == 'p') out += "pi";
            else if (it->constantName == 'f') out += "phi";
            else out += "e";
            break;
        case ItemType::Function:
            if (it->functionId == SciIntegrate) {
                out += "\xE2\x88\xAB(";
            } else if (it->functionId == SciDiff) {
                out += "d/dx(";
            } else if (it->functionId == SciSum) {
                out += "\xCE\xA3(";
            } else if (it->functionId == SciProduct) {
                out += "\xE2\x88\x8F(";
            } else {
                out += sciFunctionName(it->functionId);
                out += '(';
            }
            serializeRow(it->a.get(), out);
            out += ')';
            break;
        case ItemType::Permutation:
        case ItemType::Combination: {
            char opLetter = (it->type == ItemType::Permutation) ? 'P' : 'C';
            std::string nStr, rStr;
            if (isSimpleNumberRow(it->a.get(), nStr) && isSimpleNumberRow(it->b.get(), rStr)) {
                out += toSuperscriptString(nStr);
                out += opLetter;
                out += toSubscriptString(rStr);
            } else {
                out += '(';
                serializeRow(it->a.get(), out);
                out += ')';
                out += opLetter;
                out += '(';
                serializeRow(it->b.get(), out);
                out += ')';
            }
            break;
        }
        case ItemType::Summation:
        case ItemType::Product:
        case ItemType::Integral: {
            if (it->type == ItemType::Summation) out += "\xCE\xA3";
            else if (it->type == ItemType::Product) out += "\xE2\x88\x8F";
            else out += "\xE2\x88\xAB";
            if (!rowIsEmpty(it->b.get()) || !rowIsEmpty(it->c.get())) {
                out += "_(";
                serializeRow(it->b.get(), out);
                out += ")^(";
                serializeRow(it->c.get(), out);
                out += ")";
            }
            out += "(";
            serializeRow(it->a.get(), out);
            out += ")d";
            out += (it->variableName ? it->variableName : 'x');
            break;
        }
        case ItemType::Derivative: {
            out += "d/d";
            out += (it->variableName ? it->variableName : 'x');
            out += "(";
            serializeRow(it->a.get(), out);
            out += ")";
            if (!rowIsEmpty(it->b.get())) {
                out += "|_(";
                out += (it->variableName ? it->variableName : 'x');
                out += "=";
                serializeRow(it->b.get(), out);
                out += ")";
            }
            break;
        }
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

std::unique_ptr<Row> cloneRow(const Row* source, Item* owner, Row* parent) {
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
        item->isBracket = sourceItem->isBracket;
        item->isBrace = sourceItem->isBrace;
        Item* itemPtr = item.get();
        if (sourceItem->a) item->a = cloneRow(sourceItem->a.get(), itemPtr, copy.get());
        if (sourceItem->b) item->b = cloneRow(sourceItem->b.get(), itemPtr, copy.get());
        if (sourceItem->c) item->c = cloneRow(sourceItem->c.get(), itemPtr, copy.get());
        if (sourceItem->d) item->d = cloneRow(sourceItem->d.get(), itemPtr, copy.get());
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
    if (name != 'x' && name != 'y' && name != 'i') return;
    Row* row = expr.cursor.row;
    int idx = expr.cursor.index;
    auto item = std::make_unique<Item>(ItemType::Variable);
    item->variableName = name;
    row->items.insert(row->items.begin() + idx, std::move(item));
    expr.cursor.index = idx + 1;
}

void insertOperator(Expression& expr, char op) {
    if (op == ',' || op == ';') {
        while (expr.cursor.row && expr.cursor.row->owner &&
               (expr.cursor.row->owner->type == ItemType::Fraction ||
                expr.cursor.row->owner->type == ItemType::Power ||
                expr.cursor.row->owner->type == ItemType::Permutation ||
                expr.cursor.row->owner->type == ItemType::Combination)) {
            moveRight(expr);
        }
    } else if (isBRow(expr.cursor.row) && expr.cursor.row->owner &&
        expr.cursor.row->owner->type == ItemType::Power &&
        expr.cursor.index == (int)expr.cursor.row->items.size() &&
        (op != '-' || !expr.cursor.row->items.empty())) {
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
    if (item->c) item->c->ownerParentRow = newParentRow;
    if (item->d) item->d->ownerParentRow = newParentRow;
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

void insertPermutation(Expression& expr) {
    if (isBRow(expr.cursor.row) && expr.cursor.row->owner &&
        (expr.cursor.row->owner->type == ItemType::Power ||
         expr.cursor.row->owner->type == ItemType::Permutation ||
         expr.cursor.row->owner->type == ItemType::Combination) &&
        expr.cursor.index == (int)expr.cursor.row->items.size()) {
        moveRight(expr);
    }
    Row* row = expr.cursor.row;
    int insertPos = expr.cursor.index;

    auto pItem = std::make_unique<Item>(ItemType::Permutation);
    Item* pPtr = pItem.get();

    std::unique_ptr<Item> consumed = consumeLeftAtom(expr);
    insertPos = expr.cursor.index;

    attachRow(pItem->a, pPtr, row);
    attachRow(pItem->b, pPtr, row);
    if (consumed) {
        reparentItemChildren(consumed.get(), pItem->a.get());
        pItem->a->items.push_back(std::move(consumed));
    }

    row->items.insert(row->items.begin() + insertPos, std::move(pItem));

    // cursor -> start of subscript r
    expr.cursor.row = pPtr->b.get();
    expr.cursor.index = 0;
}

void insertCombination(Expression& expr) {
    if (isBRow(expr.cursor.row) && expr.cursor.row->owner &&
        (expr.cursor.row->owner->type == ItemType::Power ||
         expr.cursor.row->owner->type == ItemType::Permutation ||
         expr.cursor.row->owner->type == ItemType::Combination) &&
        expr.cursor.index == (int)expr.cursor.row->items.size()) {
        moveRight(expr);
    }
    Row* row = expr.cursor.row;
    int insertPos = expr.cursor.index;

    auto cItem = std::make_unique<Item>(ItemType::Combination);
    Item* cPtr = cItem.get();

    std::unique_ptr<Item> consumed = consumeLeftAtom(expr);
    insertPos = expr.cursor.index;

    attachRow(cItem->a, cPtr, row);
    attachRow(cItem->b, cPtr, row);
    if (consumed) {
        reparentItemChildren(consumed.get(), cItem->a.get());
        cItem->a->items.push_back(std::move(consumed));
    }

    row->items.insert(row->items.begin() + insertPos, std::move(cItem));

    // cursor -> start of subscript r
    expr.cursor.row = cPtr->b.get();
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

void insertOpenBracket(Expression& expr) {
    Row* row = expr.cursor.row;
    int idx = expr.cursor.index;

    auto pItem = std::make_unique<Item>(ItemType::Paren);
    pItem->isBracket = true;
    Item* pPtr = pItem.get();
    attachRow(pItem->a, pPtr, row);

    row->items.insert(row->items.begin() + idx, std::move(pItem));
    expr.cursor.row = pPtr->a.get();
    expr.cursor.index = 0;
}

void insertOpenBrace(Expression& expr) {
    Row* row = expr.cursor.row;
    int idx = expr.cursor.index;

    auto pItem = std::make_unique<Item>(ItemType::Paren);
    pItem->isBrace = true;
    Item* pPtr = pItem.get();
    attachRow(pItem->a, pPtr, row);

    row->items.insert(row->items.begin() + idx, std::move(pItem));
    expr.cursor.row = pPtr->a.get();
    expr.cursor.index = 0;
}

void insertIntegral(Expression& expr) {
    Row* row = expr.cursor.row;
    int idx = expr.cursor.index;

    auto item = std::make_unique<Item>(ItemType::Integral);
    Item* ptr = item.get();
    ptr->variableName = 'x';
    attachRow(item->a, ptr, row); // integrand
    attachRow(item->b, ptr, row); // lower limit
    attachRow(item->c, ptr, row); // upper limit

    row->items.insert(row->items.begin() + idx, std::move(item));
    expr.cursor.row = ptr->a.get();
    expr.cursor.index = 0;
}

void insertDerivative(Expression& expr) {
    Row* row = expr.cursor.row;
    int idx = expr.cursor.index;

    auto item = std::make_unique<Item>(ItemType::Derivative);
    Item* ptr = item.get();
    ptr->variableName = 'x';
    attachRow(item->a, ptr, row); // expression
    attachRow(item->b, ptr, row); // eval point (optional)

    row->items.insert(row->items.begin() + idx, std::move(item));
    expr.cursor.row = ptr->a.get();
    expr.cursor.index = 0;
}

void insertSummation(Expression& expr) {
    Row* row = expr.cursor.row;
    int idx = expr.cursor.index;

    auto item = std::make_unique<Item>(ItemType::Summation);
    Item* ptr = item.get();
    attachRow(item->a, ptr, row); // body
    attachRow(item->b, ptr, row); // lower limit
    attachRow(item->c, ptr, row); // upper limit

    row->items.insert(row->items.begin() + idx, std::move(item));
    expr.cursor.row = ptr->b.get();
    expr.cursor.index = 0;
}

void insertProduct(Expression& expr) {
    Row* row = expr.cursor.row;
    int idx = expr.cursor.index;

    auto item = std::make_unique<Item>(ItemType::Product);
    Item* ptr = item.get();
    attachRow(item->a, ptr, row); // body
    attachRow(item->b, ptr, row); // lower limit
    attachRow(item->c, ptr, row); // upper limit

    row->items.insert(row->items.begin() + idx, std::move(item));
    expr.cursor.row = ptr->b.get();
    expr.cursor.index = 0;
}

void insertCloseParen(Expression& expr) {
    Row* insertionRow = expr.cursor.row;
    int insertionIndex = expr.cursor.index;
    Row* row = insertionRow;
    while (row && row->owner) {
        int k = ownerIndexInParentRow(row);
        if (k < 0) break;
        if (row->owner->type == ItemType::Paren ||
            row->owner->type == ItemType::Function ||
            row->owner->type == ItemType::Sqrt ||
            row->owner->type == ItemType::Integral ||
            row->owner->type == ItemType::Summation ||
            row->owner->type == ItemType::Product ||
            row->owner->type == ItemType::Derivative) {
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
            case ItemType::Permutation:
            case ItemType::Combination:
            case ItemType::Derivative:
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
            case ItemType::Integral:
            case ItemType::Summation:
            case ItemType::Product:
                // These enter into their main row (a) from either side
                expr.cursor.row = prev->a.get();
                expr.cursor.index = (int)prev->a->items.size();
                return;
        }
    }

    // idx == 0: step out of the current structure, if any.
    if (row->owner) {
        if (isBRow(row) && isTwoRowStructure(row->owner->type)) {
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
            case ItemType::Permutation:
            case ItemType::Combination:
            case ItemType::Paren:
            case ItemType::Sqrt:
            case ItemType::Function:
            case ItemType::Integral:
            case ItemType::Summation:
            case ItemType::Product:
            case ItemType::Derivative:
                expr.cursor.row = next->a.get();
                expr.cursor.index = 0;
                return;
        }
    }

    // idx == end of row: step out of the current structure, if any.
    if (row->owner) {
        if (isARow(row) && isTwoRowStructure(row->owner->type)) {
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
}

bool moveUp(Expression& expr) {
    Row* row = expr.cursor.row;
    if (isBRow(row)) {
        Item* owner = row->owner;
        Row* target = owner->a.get();
        expr.cursor.row = target;
        if (expr.cursor.index > (int)target->items.size())
            expr.cursor.index = (int)target->items.size();
        return true;
    }
    if (isARow(row) && row->owner && (row->owner->type == ItemType::Integral || row->owner->type == ItemType::Summation || row->owner->type == ItemType::Product)) {
        // From integrand (a) -> go to upper limit (c)
        Item* owner = row->owner;
        Row* target = owner->c.get();
        expr.cursor.row = target;
        if (expr.cursor.index > (int)target->items.size())
            expr.cursor.index = (int)target->items.size();
        return true;
    }
    return false;
}

// Mirror of moveUp: returns true if the cursor moved.
bool moveDown(Expression& expr) {
    Row* row = expr.cursor.row;
    if (isCRow(row) && row->owner && (row->owner->type == ItemType::Integral || row->owner->type == ItemType::Summation || row->owner->type == ItemType::Product)) {
        // From upper limit (c) -> go to integrand (a)
        Item* owner = row->owner;
        Row* target = owner->a.get();
        expr.cursor.row = target;
        if (expr.cursor.index > (int)target->items.size())
            expr.cursor.index = (int)target->items.size();
        return true;
    }
    if (isARow(row) && row->owner) {
        if (row->owner->type == ItemType::Integral || row->owner->type == ItemType::Summation || row->owner->type == ItemType::Product) {
            // From integrand (a) -> go to lower limit (b)
            Item* owner = row->owner;
            Row* target = owner->b.get();
            expr.cursor.row = target;
            if (expr.cursor.index > (int)target->items.size())
                expr.cursor.index = (int)target->items.size();
            return true;
        }
        if (isTwoRowStructure(row->owner->type) || row->owner->type == ItemType::Derivative) {
            Item* owner = row->owner;
            Row* target = owner->b.get();
            expr.cursor.row = target;
            if (expr.cursor.index > (int)target->items.size())
                expr.cursor.index = (int)target->items.size();
            return true;
        }
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
        bool siblingHasContent = isTwoRowStructure(ownerItem->type) &&
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
    bool bEmpty = isTwoRowStructure(target->type)
                      ? rowIsEmpty(target->b.get())
                      : true;
    if (aEmpty && bEmpty) {
        row->items.erase(row->items.begin() + (idx - 1));
        expr.cursor.index = idx - 1;
        return;
    }

    // Non-empty structure: first backspace "enters" it (lands at the end
    // of its rightmost row) rather than deleting its contents outright.
    Row* enter = isTwoRowStructure(target->type)
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

        if (isARow(row) && isTwoRowStructure(ownerItem->type)) {
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
    bool bEmpty = isTwoRowStructure(target->type)
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
    if (which != 'p' && which != 'e' && which != 'f') return;
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
        } else if (lower == "phi") {
            it->type = ItemType::Constant; it->constantName = 'f'; it->nameText.clear();
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
        } else if (lower == "i") {
            it->type = ItemType::Variable; it->variableName = 'i'; it->nameText.clear();
            changed = true;
        } else if (lower == "union") {
            it->type = ItemType::Operator; it->opChar = 'U'; it->nameText.clear();
            changed = true;
        } else if (lower == "u") {
            // "U" converts to union operator
            it->type = ItemType::Operator; it->opChar = 'U'; it->nameText.clear();
            changed = true;
        } else if (lower == "inter" || lower == "intersection") {
            it->type = ItemType::Operator; it->opChar = 'I'; it->nameText.clear();
            changed = true;
        } else if (lower == "delta") {
            it->type = ItemType::Operator; it->opChar = 'D'; it->nameText.clear();
            changed = true;
        }
    }

    // Permutation & Combination conversion: [Atom N] [Name "p"|"P"|"npr"|"c"|"C"|"ncr"] [Atom R]
    for (size_t i = 0; i < row->items.size(); ++i) {
        if (row->items[i]->type != ItemType::Name) continue;
        const std::string lower = lowerName(row->items[i]->nameText);
        bool isP = (lower == "p" || lower == "npr");
        bool isC = (lower == "c" || lower == "ncr");
        if ((isP || isC) && i > 0 && i + 1 < row->items.size()) {
            Item* prev = row->items[i - 1].get();
            Item* next = row->items[i + 1].get();
            bool prevOk = prev && prev->type != ItemType::Operator && prev->type != ItemType::Equals && prev->type != ItemType::Name;
            bool nextOk = next && next->type != ItemType::Operator && next->type != ItemType::Equals;
            if (prevOk && nextOk) {
                auto combItem = std::make_unique<Item>(isP ? ItemType::Permutation : ItemType::Combination);
                Item* combPtr = combItem.get();
                attachRow(combItem->a, combPtr, row);
                attachRow(combItem->b, combPtr, row);
                reparentItemChildren(prev, combItem->a.get());
                combItem->a->items.push_back(std::move(row->items[i - 1]));
                reparentItemChildren(next, combItem->b.get());
                combItem->b->items.push_back(std::move(row->items[i + 1]));

                if (expr.cursor.row == row) {
                    if (expr.cursor.index == (int)(i + 1) || expr.cursor.index == (int)(i + 2)) {
                        expr.cursor.row = combPtr->b.get();
                        expr.cursor.index = (int)combPtr->b->items.size();
                    } else if (expr.cursor.index > (int)(i + 1)) {
                        expr.cursor.index -= 2;
                    }
                }
                row->items[i - 1] = std::move(combItem);
                row->items.erase(row->items.begin() + i + 1); // remove next
                row->items.erase(row->items.begin() + i);     // remove Name
                changed = true;
                --i;
                continue;
            }
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
            if (it->c) walk(it->c.get());
            if (it->d) walk(it->d.get());
        }
        normalizeRowNames(expr, row);
    };
    walk(expr.root.get());
    if (expr.cursor.row)
        expr.cursor.index = std::clamp(expr.cursor.index, 0, (int)expr.cursor.row->items.size());
}

static bool startsWithCaseInsensitive(const std::string& text, size_t k, const char* prefix, size_t len) {
    if (text.size() - k < len) return false;
    for (size_t i = 0; i < len; ++i) {
        if (lowerChar(text[k + i]) != lowerChar(prefix[i])) return false;
    }
    return true;
}

static size_t matchNameToken(const std::string& text, size_t k, int& functionId, bool& isSqrt,
                             bool& isPi, bool& isPhi) {
    functionId = -1; isSqrt = false; isPi = false; isPhi = false;
    size_t best = 0;
    for (int f = 0; f < SciFunctionCount; ++f) {
        const char* name = sciFunctionName(f);
        size_t len = std::strlen(name);
        if (len > best && startsWithCaseInsensitive(text, k, name, len)) {
            best = len; functionId = f; isSqrt = false; isPi = false; isPhi = false;
        }
    }
    static const struct { const char* alias; int id; } kAliases[] = {
        { "arcsin", SciAsin }, { "arccos", SciAcos }, { "arctan", SciAtan },
        { "cosec", SciCsc }, { "arcsec", SciAsec }, { "arccsc", SciAcsc }, { "arccosec", SciAcsc },
        { "arccot", SciAcot }, { "arcsinh", SciAsinh }, { "arccosh", SciAcosh },
        { "arctanh", SciAtanh }, { "arcsech", SciAsech }, { "arccsch", SciAcsch },
        { "arccoth", SciAcoth }, { "log10", SciLog }, { "ceiling", SciCeil },
        { "signum", SciSgn }, { "sign", SciSgn }, { "lngamma", SciLgamma },
        { "factorial", SciFact }, { "degrees", SciDeg }, { "todeg", SciDeg },
        { "radians", SciRad }, { "torad", SciRad },
        // Calculus
        { "int", SciIntegrate }, { "integral", SciIntegrate },
        { "derivative", SciDiff }, { "lim", SciLimit },
        { "sigma", SciSum }, { "prod", SciProduct },
        // Matrix
        { "inverse", SciInv }, { "trans", SciTranspose }, { "tr", SciTrace },
        { "mag", SciNorm }, { "magnitude", SciNorm }, { "identity", SciEye },
        // Stats
        { "avg", SciMean }, { "average", SciMean },
        { "stdev", SciStddev }, { "variance", SciVar },
        // Number Theory
        { "hcf", SciGcd }, { "prime", SciIsPrime },
        { "comb", SciNcrFn }, { "combinations", SciNcrFn },
        { "perm", SciNprFn }, { "permutations", SciNprFn },
        // Advanced
        { "j0", SciBesselJ0 }, { "j1", SciBesselJ1 },
        { "y0", SciBesselY0 }, { "y1", SciBesselY1 },
        { "lambert", SciLambertW }
    };
    for (const auto& a : kAliases) {
        size_t len = std::strlen(a.alias);
        if (len > best && startsWithCaseInsensitive(text, k, a.alias, len)) {
            best = len; functionId = a.id; isSqrt = false; isPi = false; isPhi = false;
        }
    }
    if (4 > best && startsWithCaseInsensitive(text, k, "sqrt", 4)) {
        best = 4; functionId = -1; isSqrt = true; isPi = false; isPhi = false;
    }
    if (2 > best && startsWithCaseInsensitive(text, k, "pi", 2)) {
        best = 2; functionId = -1; isSqrt = false; isPi = true; isPhi = false;
    }
    if (3 > best && startsWithCaseInsensitive(text, k, "phi", 3)) {
        best = 3; functionId = -1; isSqrt = false; isPi = false; isPhi = true;
    }
    return best;
}

void insertFromText(Expression& expr, const std::string& text) {
    size_t i = 0;
    while (i < text.size()) {
        // UTF-8 superscript digits:
        // ¹ (\xC2\xB9), ² (\xC2\xB2), ³ (\xC2\xB3)
        if (i + 1 < text.size() && (unsigned char)text[i] == 0xC2) {
            unsigned char b2 = (unsigned char)text[i + 1];
            if (b2 == 0xB9) { insertDigit(expr, '1'); i += 2; continue; }
            if (b2 == 0xB2) { insertDigit(expr, '2'); i += 2; continue; }
            if (b2 == 0xB3) { insertDigit(expr, '3'); i += 2; continue; }
        }
        // ⁰ (\xE2\x81\xB0) through ⁹ (\xE2\x81\xB9)
        if (i + 2 < text.size() && (unsigned char)text[i] == 0xE2 &&
            (unsigned char)text[i + 1] == 0x81) {
            unsigned char b3 = (unsigned char)text[i + 2];
            if (b3 == 0xB0) { insertDigit(expr, '0'); i += 3; continue; }
            if (b3 >= 0xB4 && b3 <= 0xB9) {
                insertDigit(expr, (char)('4' + (b3 - 0xB4)));
                i += 3;
                continue;
            }
        }
        // UTF-8 subscript digits: ₀ (\xE2\x82\x80) through ₉ (\xE2\x82\x89)
        if (i + 2 < text.size() && (unsigned char)text[i] == 0xE2 &&
            (unsigned char)text[i + 1] == 0x82) {
            unsigned char b3 = (unsigned char)text[i + 2];
            if (b3 >= 0x80 && b3 <= 0x89) {
                insertDigit(expr, (char)('0' + (b3 - 0x80)));
                i += 3;
                continue;
            }
        }
        // UTF-8 π (\xCF\x80) and φ (\xCF\x86)
        if (i + 1 < text.size() && (unsigned char)text[i] == 0xCF) {
            unsigned char b2 = (unsigned char)text[i + 1];
            if (b2 == 0x80) { insertConstant(expr, 'p'); i += 2; continue; }
            if (b2 == 0x86) { insertConstant(expr, 'f'); i += 2; continue; }
        }
        // UTF-8 ∫ (\xE2\x88\xAB)
        if (i + 2 < text.size() && (unsigned char)text[i] == 0xE2 &&
            (unsigned char)text[i + 1] == 0x88 && (unsigned char)text[i + 2] == 0xAB) {
            i += 3;
            if (i < text.size() && text[i] == '(') {
                insertFunction(expr, SciIntegrate);
                ++i;
            } else {
                insertIntegral(expr);
            }
            continue;
        }
        // UTF-8 ∪ (\xE2\x88\xAA)
        if (i + 2 < text.size() && (unsigned char)text[i] == 0xE2 &&
            (unsigned char)text[i + 1] == 0x88 && (unsigned char)text[i + 2] == 0xAA) {
            insertOperator(expr, 'U');
            i += 3;
            continue;
        }
        // UTF-8 ∩ (\xE2\x88\xA9)
        if (i + 2 < text.size() && (unsigned char)text[i] == 0xE2 &&
            (unsigned char)text[i + 1] == 0x88 && (unsigned char)text[i + 2] == 0xA9) {
            insertOperator(expr, 'I');
            i += 3;
            continue;
        }
        // UTF-8 Δ (\xCE\x94)
        // UTF-8 Σ (\xCE\xA3)
        if (i + 1 < text.size() && (unsigned char)text[i] == 0xCE &&
            (unsigned char)text[i + 1] == 0xA3) {
            i += 2;
            if (i < text.size() && text[i] == '(') {
                insertFunction(expr, SciSum);
                ++i;
            } else {
                insertFunction(expr, SciSum);
            }
            continue;
        }
        // UTF-8 Π (\xE2\x88\x8F)
        if (i + 2 < text.size() && (unsigned char)text[i] == 0xE2 &&
            (unsigned char)text[i + 1] == 0x88 && (unsigned char)text[i + 2] == 0x8F) {
            i += 3;
            if (i < text.size() && text[i] == '(') {
                insertFunction(expr, SciProduct);
                ++i;
            } else {
                insertFunction(expr, SciProduct);
            }
            continue;
        }
        if (i + 1 < text.size() && (unsigned char)text[i] == 0xCE &&
            (unsigned char)text[i + 1] == 0x94) {
            insertOperator(expr, 'D');
            i += 2;
            continue;
        }
        if (startsWithCaseInsensitive(text, i, "d/dx", 4)) {
            i += 4;
            if (i < text.size() && text[i] == '(') {
                insertFunction(expr, SciDiff);
                ++i;
            } else {
                insertDerivative(expr);
            }
            continue;
        }
        if (startsWithCaseInsensitive(text, i, "union", 5)) {
            insertOperator(expr, 'U');
            i += 5;
            continue;
        }
        if (startsWithCaseInsensitive(text, i, "inter", 5)) {
            size_t len = 5;
            if (startsWithCaseInsensitive(text, i, "intersection", 12)) len = 12;
            insertOperator(expr, 'I');
            i += len;
            continue;
        }
        if (startsWithCaseInsensitive(text, i, "delta", 5)) {
            insertOperator(expr, 'D');
            i += 5;
            continue;
        }

        char c = text[i];
        if ((c >= '0' && c <= '9') || c == '.') { insertDigit(expr, c); ++i; continue; }
        if (c == '+' || c == '-' || c == '*' || c == '!' || c == '%' || c == ',' || c == ';') { insertOperator(expr, c); ++i; continue; }
        if (c == 'U') { insertOperator(expr, 'U'); ++i; continue; }
        if (c == '/') { insertFraction(expr); ++i; continue; }
        if (c == '=') { insertEquals(expr); ++i; continue; }
        if (c == '(') { insertOpenParen(expr); ++i; continue; }
        if (c == ')') { insertCloseParen(expr); ++i; continue; }
        if (c == '[') { insertOpenBracket(expr); ++i; continue; }
        if (c == ']') { insertCloseParen(expr); ++i; continue; }
        if (c == '{') { insertOpenBrace(expr); ++i; continue; }
        if (c == '}') { insertCloseParen(expr); ++i; continue; }
        if (c == '^') { insertPower(expr); ++i; continue; }

        if (startsWithCaseInsensitive(text, i, "npr", 3)) {
            if (!(i + 3 < text.size() && text[i + 3] == '(')) {
                insertPermutation(expr);
                i += 3;
                continue;
            }
        }
        if (startsWithCaseInsensitive(text, i, "ncr", 3)) {
            if (!(i + 3 < text.size() && text[i + 3] == '(')) {
                insertCombination(expr);
                i += 3;
                continue;
            }
        }

        int functionId = -1;
        bool isSqrt = false, isPi = false, isPhi = false;
        size_t fnLen = matchNameToken(text, i, functionId, isSqrt, isPi, isPhi);
        if (fnLen > 0) {
            if (isPi) {
                insertConstant(expr, 'p');
            } else if (isPhi) {
                insertConstant(expr, 'f');
            } else if (isSqrt) {
                insertSqrt(expr);
                if (i + fnLen < text.size() && text[i + fnLen] == '(') fnLen++;
            } else if (functionId == SciSum) {
                insertSummation(expr);
                if (i + fnLen < text.size() && text[i + fnLen] == '(') fnLen++;
            } else if (functionId == SciProduct) {
                insertProduct(expr);
                if (i + fnLen < text.size() && text[i + fnLen] == '(') fnLen++;
            } else if (functionId == SciIntegrate) {
                insertIntegral(expr);
                if (i + fnLen < text.size() && text[i + fnLen] == '(') fnLen++;
            } else {
                insertFunction(expr, functionId);
                if (i + fnLen < text.size() && text[i + fnLen] == '(') fnLen++;
            }
            i += fnLen;
            continue;
        }

        char lc = lowerChar(c);
        if (lc == 'x' || lc == 'y' || lc == 'i') {
            insertVariable(expr, lc);
            ++i;
            continue;
        }
        if (lc == 'e') {
            insertConstant(expr, 'e');
            ++i;
            continue;
        }
        if (lc == 'p') {
            insertPermutation(expr);
            ++i;
            continue;
        }
        if (lc == 'c') {
            insertCombination(expr);
            ++i;
            continue;
        }

        ++i; // unknown character dropped
    }
    normalizeNames(expr);
}
