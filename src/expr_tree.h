// expr_tree.h
//
// Structural mathematical expression model.
//
// An expression is NOT a flat string. It is a "Row" of Items, in the style
// of natural-textbook-display calculators (Casio fx-991ES Plus) and
// structured math editors (MathQuill/TeX list model).
//
// A Row is an ordered sequence of Items. Most Items are simple (a digit
// run, an operator glyph). Some Items are STRUCTURAL and themselves own one
// or two child Rows (a Fraction owns a numerator Row and a denominator Row;
// a Power owns a base Row and an exponent Row; a Sqrt owns a radicand Row;
// Parens own an inner Row). This lets the cursor live "inside" a fraction's
// denominator, know how to walk out of it, and lets deletion/backspace
// understand structure instead of corrupting a flat string.
//
// The renderer (layout.h) and the evaluator (evaluator.h) both walk this
// same tree -- there is exactly one source of truth.

#pragma once
#include <vector>
#include <string>
#include <memory>

enum class ItemType {
    Number,     // a run of digits and at most one '.'
    Variable,   // a named numeric variable (currently x or y)
    Operator,   // + - * ! (leaf glyph; division never appears here, see Fraction)
    Equals,     // equation separator
    CloseParen, // standalone ')' delimiter
    Fraction,   // a/b  -> child rows: numerator (a), denominator (b)
    Paren,      // (a)  -> child row: inner (a).  b unused.
    Power,      // a^b  -> child rows: base (a), exponent (b)
    Sqrt,       // sqrt(a) -> child row: radicand (a). b unused.
    Name,       // a word being typed ("sin", "ex"...). Resolved by
                // normalizeNames() into Function/Constant/Variable, or kept
                // as-is (evaluates to "Unknown name")
    Constant,   // pi ('p') or e ('e') -- leaf
    Function    // sciFunction(id)(arg) -> child row: argument (a). b unused.
};

// Scientific functions available in Calc Pro Max. The order here must match
// the ActSin..ActAbs action order in main.cpp.
enum SciFunction {
    SciSin, SciCos, SciTan,
    SciAsin, SciAcos, SciAtan,
    SciSinh, SciCosh, SciTanh,
    SciLn, SciLog, SciExp, SciAbs,
    SciFunctionCount
};

const char* sciFunctionName(int id);   // "sin", "cos", ... (lowercase)
bool findSciFunction(const std::string& lowerName, int& id);

struct Row;

struct Item {
    ItemType type;

    // Number
    std::string numText;   // e.g. "12.5"

    // Variable
    char variableName = 0; // 'x' or 'y'

    // Operator
    char opChar = 0;       // '+', '-', '*'

    // Name
    std::string nameText;  // word as typed, e.g. "sin"

    // Constant
    char constantName = 0; // 'p' = pi, 'e' = e

    // Function
    int functionId = -1;   // SciFunction enum value

    // Structural children. Meaning depends on `type`:
    //   Fraction: a = numerator,   b = denominator
    //   Power:    a = base,        b = exponent
    //   Paren:    a = inner,       b = unused (nullptr)
    //   Sqrt:     a = radicand,    b = unused (nullptr)
    std::unique_ptr<Row> a;
    std::unique_ptr<Row> b;

    explicit Item(ItemType t) : type(t) {}
};

struct Row {
    std::vector<std::unique_ptr<Item>> items;

    // Back-link to the Item that owns this row (nullptr for the top-level
    // root row of the whole expression). Used to walk the cursor "out" of
    // a structure and to find sibling rows for vertical (up/down) motion.
    Item* owner = nullptr;
    Row*  ownerParentRow = nullptr; // the row that `owner` itself lives in
};

// A cursor is a (row, index) gap position: it sits *before* items[index]
// in `row`. index == items.size() means "at the end of this row".
struct Cursor {
    Row* row = nullptr;
    int  index = 0;
};

// The whole editable expression. Owns the root row.
struct Expression {
    std::unique_ptr<Row> root;
    Cursor cursor;

    Expression();

    // Serialize back to a linear ASCII string, e.g. for the history log
    // or for debugging ("1+2/3" style, with (num)/(den) parenthesized
    // fractions). Not used for evaluation -- evaluation walks the tree.
    std::string toPlainString() const;
};

// Copy an expression tree for history recall without sharing editable state.
std::unique_ptr<Expression> cloneExpression(const Expression& source);

std::string rowRangeToPlainString(const Row* row, int begin, int end);
void deleteRange(Expression& expression, Cursor first, Cursor last);

// ---- Editing operations -------------------------------------------------
// All operations act on `expr.cursor` and mutate the tree in place.

// Insert a digit or '.' at the cursor, extending/starting a Number item.
void insertDigit(Expression& expr, char digit);

// Insert a named variable at the cursor.
void insertVariable(Expression& expr, char name);

// Insert a flat operator (+, -, *) as its own Item at the cursor.
void insertOperator(Expression& expr, char op);

// Insert an equation separator. Enter still submits the current line.
void insertEquals(Expression& expr);

// '/' key: wrap the atom immediately to the left of the cursor (or start
// an empty one) into a Fraction, cursor moves into the denominator.
void insertFraction(Expression& expr);

// '^' key: wrap the atom to the left of the cursor into a Power, cursor
// moves into the exponent.
void insertPower(Expression& expr);

// sqrt button: insert an empty Sqrt at the cursor, cursor moves inside.
void insertSqrt(Expression& expr);

// Scientific keys: insert an empty function call, cursor moves into the
// argument; or insert the pi / e constant leaf.
void insertFunction(Expression& expr, int functionId);
void insertConstant(Expression& expr, char which); // 'p' = pi, 'e' = e

// Pro Mode typing: append a (lowercase) letter to a trailing Name item, or
// start a new one. Letters accumulate as a Name; the conversion into
// Function/Constant/Variable happens in normalizeNames() on the next
// non-letter edit, which is what makes typing "e-x-p-(" resolve to exp()
// while a lone "e" still becomes the constant.
void insertNameLetter(Expression& expr, char letter);

// Resolve every Name item in the tree: merge adjacent Names, convert
// Name+Paren ("sin(") into Function/Sqrt calls, and turn standalone
// "pi"/"e"/"x"/"y" words into constants/variables. Keeps the cursor sane.
void normalizeNames(Expression& expr);

// Build a tree from calculator text (digits, operators, function names,
// constants). Used for pasting, selection-replacement, and the text-mode
// editor; understands "sin(30)", "pi", "e", "sqrt(9)", case-insensitively.
void insertFromText(Expression& expr, const std::string& text);

// '(' key: insert an empty Paren at the cursor, cursor moves inside.
void insertOpenParen(Expression& expr);

// ')' key: if the cursor is inside an unclosed Paren's inner row (at the
// end of it), step out of it. Otherwise no-op (parens are structural, not
// typed as glyphs -- see note in expr_tree.cpp).
void insertCloseParen(Expression& expr);

// Cursor motion. These understand structure: entering a Fraction from the
// left steps into the start of the numerator (or into the end of the
// denominator if entering from the right), Up/Down move between numerator
// and denominator (and base/exponent), etc.
void moveLeft(Expression& expr);
void moveRight(Expression& expr);

// moveUp/moveDown return true if the cursor actually moved (i.e. there was
// a numerator<->denominator or base<->exponent sibling to move to).
// Callers that overload Up/Down for something else (e.g. recalling
// history when there's nowhere left to navigate structurally) should only
// do so when these return false.
bool moveUp(Expression& expr);
bool moveDown(Expression& expr);

// Backspace: delete the item just before the cursor. If that item is
// structural and one of its child rows is empty while the other has
// content, the structure collapses and the content is spliced back into
// the parent row (e.g. deleting an empty denominator's fraction wrapper
// promotes the numerator back into the surrounding row). Deleting into an
// empty structural item removes it entirely.
void backspace(Expression& expr);

// Delete (forward delete key). Mirror of backspace but removes the item
// just after the cursor.
void doDelete(Expression& expr);

// True if the row is completely empty (used for placeholder box rendering
// and for structural collapse logic).
bool rowIsEmpty(const Row* r);
bool hasEquals(const Row* r);
