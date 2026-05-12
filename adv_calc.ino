/*
 * adv_calc - v1.0
 * Scientific calculator for M5Stack Cardputer ADV
 *
 * Dependencies:
 *   - M5Cardputer >= 1.1.1  (includes M5Unified + M5GFX)
 *
 * Board settings (Arduino IDE):
 *   Board:           ESP32S3 Dev Module
 *   Flash Size:      8MB
 *   Partition Scheme: 8M with spiffs
 */

#include <M5Cardputer.h>
#include <math.h>

// ─────────────────────────────────────────────
//  Colour palette  (RGB565)
// ─────────────────────────────────────────────
#define C_BG        0x0841   // 0x0e0e0e-ish dark
#define C_BAR       0x1082   // 0x222222
#define C_LINE      0x3186   // 0x323232
#define C_WHITE     0xFFFF
#define C_GREEN     0x07E0

// ─────────────────────────────────────────────
//  Display geometry
// ─────────────────────────────────────────────
#define SCR_W       240
#define SCR_H       135

// Title bar: y=0..17  (18 px)
// Expr line:  y=18..61 (cursor at y=23, font ~18px)
// Separator:  y=62
// Result:     y=63..116
// Info bar:   y=117..134
#define Y_TITLE     3
#define Y_EXPR      23
#define Y_SEP       62
#define Y_RESULT    71
#define Y_INFO      117

// ─────────────────────────────────────────────
//  State
// ─────────────────────────────────────────────
String eq        = "";
int    cursorPos = 0;

bool   menuActive = false;
bool   helpActive = false;
int    mIdx       = 0;

const char* menuItems[]  = { "Sin()", "Cos()", "Tan()", "Rad()", "Pi", "E", "Sqrt()" };
const char* menuInsert[] = { "Sin(", "Cos(", "Tan(", "Rad(", "Pi", "E", "Sqrt(" };
const int   MENU_LEN     = 7;
// Does the inserted text end with '(' → place cursor before ')'?
const bool  menuHasParen[] = { true, true, true, true, false, false, true };

String resultStr = "";

// Cursor blink
unsigned long lastBlink  = 0;
bool          cursorOn   = true;
#define BLINK_MS 500

// ─────────────────────────────────────────────
//  Forward declarations
// ─────────────────────────────────────────────
void drawInterface();
void drawHelp();
void drawDropdown();
void updateExprLine();
String solveExpression(String expr);

// ─────────────────────────────────────────────
//  Tiny recursive-descent expression parser
//  Supports: +  -  *  /  ^  unary-  ()
//            sin cos tan rad sqrt pi e
//            implicit multiplication: 2pi  3(x)  etc.
// ─────────────────────────────────────────────
struct Parser {
    const char* s;
    int         pos;

    char peek() { return s[pos]; }
    char consume() { return s[pos++]; }

    void skipSpaces() {
        while (s[pos] == ' ') pos++;
    }

    // Returns NaN on error — caller checks with isnan()
    double parseExpr();
    double parseTerm();
    double parseFactor();
    double parsePrimary();
    double parseNumber();
    double parseFunc(const char* name);
    bool   matchWord(const char* word);
};

bool Parser::matchWord(const char* word) {
    int len = strlen(word);
    if (strncasecmp(s + pos, word, len) == 0) {
        // Make sure it's not a prefix of something longer
        char next = s[pos + len];
        if (!isalpha(next) && next != '_') {
            pos += len;
            return true;
        }
    }
    return false;
}

double Parser::parseNumber() {
    skipSpaces();
    int start = pos;
    // optional digits before '.'
    while (isdigit(s[pos])) pos++;
    // optional decimal part
    if (s[pos] == '.') {
        pos++;
        while (isdigit(s[pos])) pos++;
    }
    if (pos == start) return NAN;
    char buf[64];
    int len = pos - start;
    if (len >= (int)sizeof(buf)) return NAN;
    strncpy(buf, s + start, len);
    buf[len] = '\0';
    return atof(buf);
}

double Parser::parsePrimary() {
    skipSpaces();
    char c = peek();

    // Parenthesised sub-expression
    if (c == '(') {
        consume(); // '('
        double v = parseExpr();
        skipSpaces();
        if (peek() == ')') consume();
        return v;
    }

    // Unary minus
    if (c == '-') {
        consume();
        return -parseFactor();
    }
    // Unary plus
    if (c == '+') {
        consume();
        return parsePrimary();
    }

    // Named constants & functions (case-insensitive)
    if (isalpha(c)) {
        if (matchWord("pi"))   return M_PI;
        if (matchWord("e"))    return M_E;
        if (matchWord("sin"))  return parseFunc("sin");
        if (matchWord("cos"))  return parseFunc("cos");
        if (matchWord("tan"))  return parseFunc("tan");
        if (matchWord("rad"))  return parseFunc("rad");
        if (matchWord("sqrt")) return parseFunc("sqrt");
        // Unknown identifier → treat as variable 'x'=0 placeholder → NaN so
        // the equation solver path handles it
        while (isalpha(s[pos])) pos++;
        return NAN;
    }

    // Number
    return parseNumber();
}

double Parser::parseFunc(const char* name) {
    skipSpaces();
    double inner = parseFactor(); // will consume '(' ... ')'
    if (isnan(inner)) return NAN;
    if (strcmp(name, "sin")  == 0) return sin(inner);
    if (strcmp(name, "cos")  == 0) return cos(inner);
    if (strcmp(name, "tan")  == 0) return tan(inner);
    if (strcmp(name, "rad")  == 0) return inner * M_PI / 180.0;
    if (strcmp(name, "sqrt") == 0) return sqrt(inner);
    return NAN;
}

double Parser::parseFactor() {
    skipSpaces();
    double base = parsePrimary();

    // Implicit multiplication: number/const directly followed by '(' or letter
    // e.g. 2pi → 2 * pi    3(x+1) → 3*(x+1)
    while (true) {
        skipSpaces();
        char c = peek();
        if (c == '(' || isalpha(c)) {
            double right = parsePrimary();
            if (isnan(right)) break;
            base *= right;
        } else {
            break;
        }
    }

    // Exponentiation (right-associative)
    skipSpaces();
    if (peek() == '^') {
        consume();
        double exp = parseFactor();   // right-associative
        base = pow(base, exp);
    }
    return base;
}

double Parser::parseTerm() {
    double left = parseFactor();
    while (true) {
        skipSpaces();
        char c = peek();
        if (c == '*' || c == '/') {
            consume();
            double right = parseFactor();
            left = (c == '*') ? left * right : left / right;
        } else {
            break;
        }
    }
    return left;
}

double Parser::parseExpr() {
    double left = parseTerm();
    while (true) {
        skipSpaces();
        char c = peek();
        if (c == '+' || c == '-') {
            consume();
            double right = parseTerm();
            left = (c == '+') ? left + right : left - right;
        } else {
            break;
        }
    }
    return left;
}

// ─────────────────────────────────────────────
//  Evaluate a numeric expression string
//  Returns NAN on parse error
// ─────────────────────────────────────────────
double evalExpr(const String& str) {
    Parser p;
    p.s   = str.c_str();
    p.pos = 0;
    p.skipSpaces();
    double v = p.parseExpr();
    p.skipSpaces();
    // If there's leftover input (unexpected char), it's an error
    if (p.s[p.pos] != '\0') return NAN;
    return v;
}

// ─────────────────────────────────────────────
//  Format a double for display
// ─────────────────────────────────────────────
String fmtNumber(double v) {
    if (isnan(v))  return "error";
    if (isinf(v))  return v > 0 ? "inf" : "-inf";
    if (v == (long long)v && fabs(v) < 1e12) {
        // Integer
        char buf[32];
        snprintf(buf, sizeof(buf), "= %lld", (long long)v);
        return String(buf);
    }
    char buf[32];
    snprintf(buf, sizeof(buf), "=~ %.6g", v);
    return String(buf);
}

// ─────────────────────────────────────────────
//  Solve  a linear equation in one variable
//  Handles:  "3x + 2 = 8"   "ax = b"  etc.
//  Returns variable_name + " = " + value  or "error"
// ─────────────────────────────────────────────
String solveLinear(String lhsStr, String rhsStr, char var) {
    // Move everything to left: solve (lhs - rhs = 0)
    // This handles variable on both sides, e.g. 7(x+6)=x → 7(x+6)-x=0
    // sub(): replace var with rep, adding '*' for implicit multiplication
    auto sub = [&](const String& s, const String& rep) -> double {
        String result = "";
        for (int i = 0; i < (int)s.length(); i++) {
            char c = s[i];
            if (tolower(c) == var) {
                if (result.length() > 0) {
                    char prev = result[result.length() - 1];
                    if (isdigit(prev) || prev == ')') result += '*';
                }
                result += rep;
                if (i + 1 < (int)s.length()) {
                    char next = s[i + 1];
                    if (isdigit(next) || next == '(') result += '*';
                }
            } else {
                result += c;
            }
        }
        return evalExpr(result);
    };

    // combined = lhs - rhs, evaluate with var=0 and var=1
    // f(v) = sub(lhs,v) - sub(rhs,v)
    double b = sub(lhsStr, "0") - sub(rhsStr, "0"); // constant term of combined
    double a = (sub(lhsStr, "1") - sub(rhsStr, "1")) - b; // coefficient

    if (isnan(a) || isnan(b)) return "error";
    if (fabs(a) < 1e-12) return fabs(b) < 1e-9 ? "any value" : "no solution";

    double res = -b / a;
    char buf[64];
    if (fabs(res - round(res)) < 1e-9 && fabs(res) < 1e12)
        snprintf(buf, sizeof(buf), "%c = %lld", var, (long long)round(res));
    else
        snprintf(buf, sizeof(buf), "%c =~ %.4g", var, res);
    return String(buf);
}

// ─────────────────────────────────────────────
//  Main solver  (mirrors Python original)
// ─────────────────────────────────────────────
String solveExpression(String expr) {
    // Normalise
    expr.replace(":", "/");
    expr.trim();

    int eqIdx = expr.indexOf('=');
    if (eqIdx >= 0) {
        String lhs = expr.substring(0, eqIdx);
        String rhs = expr.substring(eqIdx + 1);
        lhs.trim(); rhs.trim();

        // Find variable: skip known constants/functions
        auto findVar = [](const String& s) -> char {
            // Words to skip (constants and function names)
            const char* skip[] = { "pi", "sin", "cos", "tan", "rad", "sqrt", nullptr };
            int len = s.length();
            int i = 0;
            while (i < len) {
                char c = s[i];
                if (isalpha(c)) {
                    // Try to match a known word
                    bool matched = false;
                    for (int k = 0; skip[k] != nullptr; k++) {
                        int wlen = strlen(skip[k]);
                        if (strncasecmp(s.c_str() + i, skip[k], wlen) == 0) {
                            // Make sure it's not a prefix of something longer
                            char next = (i + wlen < len) ? s[i + wlen] : '\0';
                            if (!isalpha(next)) {
                                i += wlen; // skip the whole word
                                matched = true;
                                break;
                            }
                        }
                    }
                    if (!matched) {
                        // Also skip standalone 'e' (Euler's number)
                        if (tolower(c) == 'e') {
                            char next = (i + 1 < len) ? s[i + 1] : '\0';
                            if (!isalpha(next)) { i++; continue; }
                        }
                        return tolower(c); // found a variable
                    }
                } else {
                    i++;
                }
            }
            return '\0';
        };

        char lVar = findVar(lhs);
        char rVar = findVar(rhs);
        char var = '\0';
        String varSide = "", numSide = "";

        if (lVar != '\0') {
            var = lVar; varSide = lhs; numSide = rhs;
        } else if (rVar != '\0') {
            // Variable is on rhs, e.g. "2+3=x" → solve rhs for var, lhs is rValue
            var = rVar; varSide = rhs; numSide = lhs;
        }

        if (var != '\0') {
            return solveLinear(varSide, numSide, var);
        } else {
            // Identity check: no variable on either side
            double l = evalExpr(lhs);
            double r = evalExpr(rhs);
            if (isnan(l) || isnan(r)) return "error";
            return fabs(l - r) < 1e-9 ? "True" : "False";
        }
    }

    // Plain expression
    double v = evalExpr(expr);
    if (isnan(v)) return "error";
    return fmtNumber(v);
}

// ─────────────────────────────────────────────
//  Drawing helpers
// ─────────────────────────────────────────────
void drawInterface() {
    auto& d = M5Cardputer.Display;
    d.fillScreen(C_BG);

    // Title bar
    d.fillRect(0, 0, SCR_W, 18, C_BAR);
    d.setTextColor(C_WHITE);
    d.setTextSize(1);
    d.setFont(&fonts::FreeSans9pt7b);
    d.setTextDatum(TL_DATUM);
    d.drawString("opt for help:      adv-calc-v1.0", 3, Y_TITLE);

    // Separator line
    d.drawLine(1, Y_SEP, SCR_W - 1, Y_SEP, C_LINE);

    // Cursor indicator rectangle (mirrors Python rect0)
    d.fillRect(0, Y_SEP + 9, 21, 20, C_WHITE);

    // Info bar
    d.fillRect(0, Y_INFO, SCR_W, SCR_H - Y_INFO, C_BAR);
    d.setFont(&fonts::FreeSans9pt7b);
    d.drawString("OK=solve | DEL=erase", 2, Y_INFO + 2);

    resultStr = "";
    updateExprLine();
}

void drawHelp() {
    auto& d = M5Cardputer.Display;
    d.fillScreen(C_BAR);
    d.setTextColor(C_WHITE);
    d.setFont(&fonts::FreeSansBold12pt7b);
    d.drawString("HELP MENU", 60, 5);
    d.setFont(&fonts::FreeSans9pt7b);
    d.drawString("- , / : cursor left/right",   10, 30);
    d.drawString("- Caps+Right: /  Caps+Down: .", 10, 48);
    d.drawString("- Fn: func menu  ;/. : nav",   10, 66);
    d.drawString("- Opt: toggle help",            10, 84);
}

void drawDropdown() {
    auto& d = M5Cardputer.Display;
    // Background box
    d.fillRect(140, 10, 95, MENU_LEN * 14 + 8, C_BAR);
    d.drawRect(140, 10, 95, MENU_LEN * 14 + 8, C_WHITE);
    d.setFont(&fonts::FreeSans9pt7b);
    for (int i = 0; i < MENU_LEN; i++) {
        d.setTextColor(i == mIdx ? C_GREEN : C_WHITE);
        d.drawString(menuItems[i], 145, 15 + i * 14);
    }
}

void updateExprLine() {
    auto& d = M5Cardputer.Display;
    // Clear expr area
    d.fillRect(0, 18, SCR_W, Y_SEP - 18, C_BG);

    // Build display string with blinking cursor
    String display = eq.substring(0, cursorPos)
                   + (cursorOn ? "|" : " ")
                   + eq.substring(cursorPos);

    d.setTextColor(C_WHITE);
    d.setFont(&fonts::FreeSansBold12pt7b);
    d.drawString(display, 5, Y_EXPR);

    // Result area
    d.fillRect(0, Y_SEP + 1, SCR_W, Y_INFO - Y_SEP - 1, C_BG);
    if (resultStr.length() > 0) {
        // Tweak RECT_Y_OFFSET to align rect with text on your device
        const int RECT_Y_OFFSET = 4;
        d.fillRect(0, Y_RESULT + RECT_Y_OFFSET, 5, 16, C_WHITE);
        d.drawString(resultStr, 12, Y_RESULT);
    }
}

// ─────────────────────────────────────────────
//  setup / loop
// ─────────────────────────────────────────────
void setup() {
    auto cfg = M5.config();
    M5Cardputer.begin(cfg, true);
    M5Cardputer.Display.setRotation(1);
    drawInterface();
}

bool prevFn    = false;
bool prevOpt   = false;

void loop() {
    M5Cardputer.update();

    // Cursor blink
    unsigned long now = millis();
    if (now - lastBlink >= BLINK_MS) {
        lastBlink = now;
        cursorOn  = !cursorOn;
        if (!menuActive && !helpActive) updateExprLine();
    }

    if (!M5Cardputer.Keyboard.isChange()) return;

    if (M5Cardputer.Keyboard.isPressed()) {
        Keyboard_Class::KeysState s = M5Cardputer.Keyboard.keysState();

        // ── Opt key: toggle help ─────────────────────────────────
        if (s.opt && !prevOpt) {
            helpActive = !helpActive;
            menuActive = false;
            if (helpActive) drawHelp();
            else            drawInterface();
            prevOpt = s.opt;
            return;
        }
        prevOpt = s.opt;

        if (helpActive) { prevFn = s.fn; return; }

        // ── Fn layer: only menu toggle ────────────────────────────
        if (s.fn) {
            if (!prevFn) {
                // Fn pressed fresh → toggle func menu
                menuActive = !menuActive;
                if (menuActive) drawDropdown();
                else            drawInterface();
            }
            prevFn = true;
            return;
        }
        prevFn = false;

        // ── Menu navigation ───────────────────────────────────────
        if (menuActive) {
            if (s.enter) {
                String ins = String(menuInsert[mIdx]);
                if (menuHasParen[mIdx]) {
                    ins += ")";
                    eq = eq.substring(0, cursorPos) + ins + eq.substring(cursorPos);
                    cursorPos += ins.length() - 1;
                } else {
                    eq = eq.substring(0, cursorPos) + ins + eq.substring(cursorPos);
                    cursorPos += ins.length();
                }
                menuActive = false;
                drawInterface();
            } else {
                for (auto c : s.word) {
                    if (c == ';') { // up arrow
                        mIdx = (mIdx - 1 + MENU_LEN) % MENU_LEN;
                        drawDropdown();
                    } else if (c == '.') { // down arrow
                        mIdx = (mIdx + 1) % MENU_LEN;
                        drawDropdown();
                    }
                }
            }
            return;
        }

        // ── Normal input ─────────────────────────────────────────

        // Backspace
        if (s.del) {
            if (cursorPos > 0) {
                eq = eq.substring(0, cursorPos - 1) + eq.substring(cursorPos);
                cursorPos--;
            }
            updateExprLine();
            return;
        }

        // Enter → solve
        if (s.enter) {
            if (eq.length() > 0)
                resultStr = solveExpression(eq);
            updateExprLine();
            return;
        }

        // Printable characters
        for (auto c : s.word) {
            // Arrow key mappings (these keys physically produce these chars):
            // ';' = up arrow   → not used in calc, skip
            // ',' = left arrow → move cursor left
            // '.' = down arrow → not used in calc, skip
            // '/' = right arrow → move cursor right
            // '?' = caps+right → insert '/'
            if (c == ',') {
                if (cursorPos > 0) { cursorPos--; }
                updateExprLine();
                continue;
            }
            if (c == '/') {
                if (cursorPos < (int)eq.length()) { cursorPos++; }
                updateExprLine();
                continue;
            }
            if (c == '?') {
                eq = eq.substring(0, cursorPos) + "/" + eq.substring(cursorPos);
                cursorPos++;
                updateExprLine();
                continue;
            }
            if (c == '>') { // caps+down arrow → decimal point
                eq = eq.substring(0, cursorPos) + "." + eq.substring(cursorPos);
                cursorPos++;
                updateExprLine();
                continue;
            }

            // Only accept calculator-valid characters
            if (isdigit(c) || isalpha(c) ||
                c == '+' || c == '-' || c == '*' ||
                c == '(' || c == ')' || c == '=' ||
                c == ' ' || c == '^') {
                // Keep original case — user types Pi/Sin with caps,
                // single-letter variables naturally come in lowercase
                eq = eq.substring(0, cursorPos) + c + eq.substring(cursorPos);
                cursorPos++;
            }
        }
        updateExprLine();

    } else {
        // All keys released
        prevFn  = false;
        prevOpt = false;
    }
}
