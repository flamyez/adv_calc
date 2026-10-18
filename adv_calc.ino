/*
 * adv_calc - v1.1
 * Scientific calculator for M5Stack Cardputer ADV
 *
 * Dependencies:
 *   - M5Cardputer >= 1.1.1  (includes M5Unified + M5GFX)
 *   - SPI, SD (bundled with ESP32 Arduino core)
 *
 * Board settings (Arduino IDE):
 *   Board:           ESP32S3 Dev Module
 *   Flash Size:      8MB
 *   Partition Scheme: 8M with spiffs
 */

#include <M5Cardputer.h>
#include <SPI.h>
#include <SD.h>
#include <math.h>

#define FW_VERSION  "v1.1"

// ─────────────────────────────────────────────
//  Colour palette  (RGB565)
// ─────────────────────────────────────────────
#define C_BG        0x0841   // dark background
#define C_BAR       0x1082   // 0x222222
#define C_LINE      0x3186   // 0x323232
#define C_WHITE     0xFFFF
#define C_GREEN     0x07E0
#define C_GRAY      0x7BEF   // history entries (readable on C_BG)

// ─────────────────────────────────────────────
//  Display geometry
// ─────────────────────────────────────────────
#define SCR_W       240
#define SCR_H       135

// Title bar:    y=0..17
// History row:  y=18..37 (prev entry left, mode/battery right)
// Expr line:    y=38..65
// Separator:    y=72
// Result:       y=73..116
// Info bar:     y=117..134
#define Y_TITLE     3
#define Y_HIST      21
#define Y_EXPR      41
#define Y_SEP       72
#define Y_RESULT    83
#define Y_INFO      117

// Sprite covers the whole middle zone (history + expr + result)
// → redraws are flicker-free
#define CANVAS_Y    18
#define CANVAS_H    (Y_INFO - CANVAS_Y)

#define BRIGHTNESS  160
#define IDLE_MS     120000   // autosleep after 2 min
#define MAXVIS      21       // visible chars of the expr line

// microSD (Cardputer ADV slot)
#define SD_SCK      40
#define SD_MISO     39
#define SD_MOSI     14
#define SD_CS       12
#define SD_DIR      "/adv_calc"
#define SD_HIST     "/adv_calc/calc_history.txt"

// ─────────────────────────────────────────────
//  State
// ─────────────────────────────────────────────
String eq        = "";
int    cursorPos = 0;
int    winStart  = 0;        // horizontal scroll window

bool   menuActive = false;
bool   helpActive = false;
int    mIdx       = 0;
int    tabIdx     = 0;

bool   degMode    = false;   // false = RAD, true = DEG

String resultStr  = "";

double lastAns    = 0;
bool   hasAns     = false;

// History (10 last entries, oldest first)
#define HIST_MAX 10
String histExpr[HIST_MAX];
String histRes[HIST_MAX];
int    histLen    = 0;
int    histBrowse = -1;      // -1 = editing; 0 = newest entry
String editBuf    = "";
bool   showPreviousExpr = false; // show the expression solved before the current one
String previousExpr = "";        // stays visible while the new calculation is shown

// Cursor blink
unsigned long lastBlink  = 0;
bool          cursorOn   = true;
#define BLINK_MS 500

// Battery / sleep
unsigned long lastBattCheck = 0;
unsigned long lastActivity  = 0;
bool          sleeping      = false;

// DEL auto-repeat (hold = fast erase)
bool          delHeld   = false;
unsigned long delStart  = 0;
unsigned long delRepeat = 0;
#define DEL_DELAY_MS  400
#define DEL_REPEAT_MS 60

bool sdOk = false;
int  batteryLevel = -1;     // sampled periodically; never during a screen redraw

// Offscreen buffer for the middle zone (anti-flicker)
M5Canvas canvas(&M5Cardputer.Display);

// ─────────────────────────────────────────────
//  Function menu (tabbed)
// ─────────────────────────────────────────────
struct MenuItem {
    const char* label;
    const char* insert;
    bool        hasParen;  // insert ends with '(' → also add ')' and put cursor inside
    bool        isAction;  // true = toggle action (mode switch), not inserted
};
struct MenuTab {
    const char* title;
    MenuItem    items[8];
    int         count;
};

MenuTab menuTabs[] = {
    { "Trig",  { {"Sin()","Sin(",true,false}, {"Cos()","Cos(",true,false},
                 {"Tan()","Tan(",true,false}, {"Rad()","Rad(",true,false} }, 4 },
    { "Math",  { {"Sqrt()","Sqrt(",true,false}, {"Log()","Log(",true,false},
                 {"Ln()","Ln(",true,false},   {"Abs()","Abs(",true,false},
                 {"Floor()","Floor(",true,false}, {"Ceil()","Ceil(",true,false},
                 {"Round()","Round(",true,false} }, 7 },
    { "Num",   { {"Gcd(a,b)","Gcd(",true,false}, {"Lcm(a,b)","Lcm(",true,false},
                 {"Factor(n)","Factor(",true,false} }, 3 },
    { "Const", { {"Pi","Pi",false,false}, {"E","E",false,false},
                 {"Ans","Ans",false,false} }, 3 },
    { "Mode",  { {"Deg/Rad","",false,true} }, 1 },
};
const int TAB_COUNT = sizeof(menuTabs) / sizeof(menuTabs[0]);

// ─────────────────────────────────────────────
//  Forward declarations
// ─────────────────────────────────────────────
void drawInterface();
void drawTitle();
void drawHelp();
void drawDropdown();
void redrawMenu();
void drawStatus();
void updateExprLine();
String solveExpression(String expr);
void doBackspace();
void insertText(const String& s, int cursorOffset);

// ─────────────────────────────────────────────
//  Expression parser (recursive descent)
//
//  expr     := term (('+'|'-') term ['%'])*
//  term     := unary (('*'|'/'|implicit) unary)*
//  unary    := ('-'|'+') unary | power
//  power    := postfix ('^' unary)?
//  postfix  := primary '!'*
//  primary  := number | const | func | '(' expr ')'
//
//  Constants:  pi  e  ans
//  Functions:  sin cos tan rad sqrt log ln abs floor ceil round gcd lcm
//  '%' after a +/- term means "percent of the left operand": 200+10% = 220
//  Errors set err/errPos instead of silently dropping input.
// ─────────────────────────────────────────────
struct Parser {
    const char* s;
    int         pos;
    bool        err;
    int         errPos;

    char peek()    { return s[pos]; }
    char consume() { return s[pos++]; }
    void skipSpaces() { while (s[pos] == ' ') pos++; }
    void fail()       { if (!err) { err = true; errPos = pos; } }
    void failAt(int p){ if (!err) { err = true; errPos = p; } }

    double parseExpr();
    double parseTerm();
    double parseUnary();
    double parsePower();
    double parsePostfix();
    double parsePrimary();
    double parseNumber();
    double parseFunc(const char* name);
    bool   matchWord(const char* word);
};

bool Parser::matchWord(const char* word) {
    int len = strlen(word);
    if (strncasecmp(s + pos, word, len) == 0) {
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
    while (isdigit(s[pos])) pos++;
    if (s[pos] == '.') {
        pos++;
        while (isdigit(s[pos])) pos++;
    }
    if (pos == start) { fail(); return NAN; }
    // Scientific notation: 1.5e3  2E-4  (only if digits follow e/E)
    if (s[pos] == 'e' || s[pos] == 'E') {
        int q = pos + 1;
        if (s[q] == '+' || s[q] == '-') q++;
        if (isdigit(s[q])) {
            pos = q;
            while (isdigit(s[pos])) pos++;
        }
    }
    char buf[64];
    int len = pos - start;
    if (len >= (int)sizeof(buf)) { fail(); return NAN; }
    strncpy(buf, s + start, len);
    buf[len] = '\0';
    return atof(buf);
}

double Parser::parsePrimary() {
    skipSpaces();
    char c = peek();

    if (c == '(') {
        consume();
        double v = parseExpr();
        skipSpaces();
        if (peek() == ')') consume();
        else fail();               // unclosed paren is an error now
        return v;
    }

    if (isalpha(c)) {
        int start = pos;
        if (matchWord("pi"))    return M_PI;
        if (matchWord("ans")) { if (hasAns) return lastAns; failAt(start); return NAN; }
        if (matchWord("sin"))   return parseFunc("sin");
        if (matchWord("cos"))   return parseFunc("cos");
        if (matchWord("tan"))   return parseFunc("tan");
        if (matchWord("rad"))   return parseFunc("rad");
        if (matchWord("sqrt"))  return parseFunc("sqrt");
        if (matchWord("log"))   return parseFunc("log");
        if (matchWord("ln"))    return parseFunc("ln");
        if (matchWord("abs"))   return parseFunc("abs");
        if (matchWord("floor")) return parseFunc("floor");
        if (matchWord("ceil"))  return parseFunc("ceil");
        if (matchWord("round")) return parseFunc("round");
        if (matchWord("gcd"))   return parseFunc("gcd");
        if (matchWord("lcm"))   return parseFunc("lcm");
        if (matchWord("e"))     return M_E;
        // Unknown identifier → hard error (never silently dropped)
        while (isalpha(s[pos])) pos++;
        failAt(start);
        return NAN;
    }

    return parseNumber();
}

static long long llGcd(long long a, long long b) {
    if (a < 0) a = -a;
    if (b < 0) b = -b;
    while (b) { long long t = a % b; a = b; b = t; }
    return a;
}

double Parser::parseFunc(const char* name) {
    skipSpaces();
    bool twoArg = (strcmp(name, "gcd") == 0 || strcmp(name, "lcm") == 0);
    double a = NAN, b = NAN;

    if (peek() == '(') {
        consume();
        a = parseExpr();
        if (err) return NAN;
        if (twoArg) {
            skipSpaces();
            if (peek() == ',') consume();
            else { fail(); return NAN; }
            b = parseExpr();
            if (err) return NAN;
        }
        skipSpaces();
        if (peek() == ')') consume();
        else { fail(); return NAN; }
    } else {
        if (twoArg) { fail(); return NAN; }
        a = parseUnary();          // sin 30, sqrt 2 etc.
        if (err) return NAN;
    }

    if (!strcmp(name, "sin"))  { double t = degMode ? a * M_PI / 180.0 : a; return sin(t); }
    if (!strcmp(name, "cos"))  { double t = degMode ? a * M_PI / 180.0 : a; return cos(t); }
    if (!strcmp(name, "tan"))  { double t = degMode ? a * M_PI / 180.0 : a; return tan(t); }
    if (!strcmp(name, "rad"))  return a * M_PI / 180.0;
    if (!strcmp(name, "sqrt")) return a < 0 ? NAN : sqrt(a);
    if (!strcmp(name, "log"))  return a <= 0 ? NAN : log10(a);
    if (!strcmp(name, "ln"))   return a <= 0 ? NAN : log(a);
    if (!strcmp(name, "abs"))  return fabs(a);
    if (!strcmp(name, "floor")) return floor(a);
    if (!strcmp(name, "ceil"))  return ceil(a);
    if (!strcmp(name, "round")) return round(a);
    if (!strcmp(name, "gcd") || !strcmp(name, "lcm")) {
        long long ia = llround(a), ib = llround(b);
        if (fabs(a - ia) > 1e-9 || fabs(b - ib) > 1e-9 ||
            fabs(a) > 2e9 || fabs(b) > 2e9) { fail(); return NAN; }
        long long g = llGcd(ia, ib);
        if (!strcmp(name, "gcd")) return (double)g;
        if (g == 0) return 0;
        double l = (double)(ia / g) * (double)ib;
        if (fabs(l) > 9e15) { fail(); return NAN; }
        return l < 0 ? -l : l;
    }
    fail();
    return NAN;
}

double Parser::parsePostfix() {
    double v = parsePrimary();
    while (!err) {
        skipSpaces();
        if (peek() != '!') break;
        consume();
        if (v < 0 || v != floor(v) || v > 170) { fail(); return NAN; }
        double r = 1;
        for (int i = 2; i <= (int)v; i++) r *= i;
        v = r;
    }
    return v;
}

double Parser::parsePower() {
    double base = parsePostfix();
    if (err) return NAN;
    skipSpaces();
    if (peek() == '^') {
        consume();
        double e = parseUnary();       // right-associative
        if (err) return NAN;
        if (base < 0 && e != floor(e)) { fail(); return NAN; }  // (-8)^(1/3)
        return pow(base, e);
    }
    return base;
}

double Parser::parseUnary() {
    skipSpaces();
    if (peek() == '-') { consume(); return -parseUnary(); }
    if (peek() == '+') { consume(); return  parseUnary(); }
    return parsePower();
}

double Parser::parseTerm() {
    double left = parseUnary();
    while (!err) {
        skipSpaces();
        char c = peek();
        if (c == '*' || c == '/') {
            consume();
            double right = parseUnary();
            left = (c == '*') ? left * right : left / right;
        } else if (c == '(' || isalpha(c) || isdigit(c)) {
            // Implicit multiplication: 2pi, 3(x+1), pi2, (2)3
            // Binds *looser* than '^', so 2Pi^2 = 2*(Pi^2)
            double right = parseUnary();
            if (!err) left *= right;
        } else {
            break;
        }
    }
    return left;
}

double Parser::parseExpr() {
    double left = parseTerm();
    while (!err) {
        skipSpaces();
        char c = peek();
        if (c == '%') {               // standalone percent: 50% = 0.5
            consume();
            left /= 100.0;
        } else if (c == '+' || c == '-') {
            consume();
            double right = parseTerm();
            skipSpaces();
            if (peek() == '%') {      // 200 + 10% → 200 + 200*0.10
                consume();
                right = left * right / 100.0;
            }
            left = (c == '+') ? left + right : left - right;
        } else {
            break;
        }
    }
    return left;
}

// ─────────────────────────────────────────────
//  Evaluate a numeric expression string
//  Returns NAN on parse error; error position is in g_evalErrPos.
//  (No custom struct in the signature: Arduino IDE auto-generates
//   prototypes BEFORE struct definitions and compilation breaks.)
// ─────────────────────────────────────────────
int g_evalErrPos = -1;

double evalCalc(const String& str) {
    Parser p;
    p.s = str.c_str(); p.pos = 0; p.err = false; p.errPos = 0;
    p.skipSpaces();
    double v = p.parseExpr();
    p.skipSpaces();
    if (!p.err && p.s[p.pos] != '\0') p.fail();
    g_evalErrPos = p.errPos;
    return (p.err || isnan(v)) ? NAN : v;
}

// ─────────────────────────────────────────────
//  Formatting
// ─────────────────────────────────────────────
// Bare value, no "= " prefix
String fmtVal(double v) {
    char buf[40];
    if (isnan(v)) return "?";
    if (isinf(v)) return v > 0 ? "inf" : "-inf";
    // Range check BEFORE the int64 cast (no UB on huge doubles)
    if (fabs(v) < 1e12 && v == floor(v)) {
        snprintf(buf, sizeof(buf), "%lld", (long long)v);
        return String(buf);
    }
    snprintf(buf, sizeof(buf), "%.6g", v);
    return String(buf);
}

// Rational approximation p/q (q <= 64) via continued fractions
static bool approxFraction(double v, long long& p, long long& q) {
    if (fabs(v) > 1e6) return false;
    double x = fabs(v);
    long long n0 = 0, d0 = 1, n1 = 1, d1 = 0;
    for (int i = 0; i < 24; i++) {
        long long a = (long long)floor(x);
        long long n2 = a * n1 + n0, d2 = a * d1 + d0;
        if (d2 > 64) break;
        n0 = n1; d0 = d1; n1 = n2; d1 = d2;
        double frac = x - floor(x);
        if (frac < 1e-12) break;
        x = 1.0 / frac;
    }
    if (d1 == 0 || d1 > 64) return false;
    double approx = (double)n1 / d1;
    if (fabs(fabs(v) - approx) < 1e-9 && d1 > 1) {
        p = (v < 0 ? -n1 : n1);
        q = d1;
        return true;
    }
    return false;
}

String fmtNumber(double v) {
    if (isnan(v)) return "error";
    if (isinf(v)) return v > 0 ? "= inf" : "= -inf";

    bool isInt = (fabs(v) < 1e12 && v == floor(v));
    String out = isInt ? "= " + fmtVal(v) : "=~ " + fmtVal(v);
    if (isInt) return out;

    // Hint: exact simple fraction
    long long p, q;
    if (approxFraction(v, p, q)) {
        out += " (" + String((long)p) + "/" + String((long)q) + ")";
        return out;
    }
    // Hint: multiple / fraction of Pi
    double r = v / M_PI;
    long long pr, qr;
    if (approxFraction(r, pr, qr)) {
        String coef;
        if (qr == 1)      coef = (pr == 1) ? "Pi" : String((long)pr) + "*Pi";
        else if (pr == 1) coef = "Pi/" + String((long)qr);
        else              coef = String((long)pr) + "*Pi/" + String((long)qr);
        out += " (" + coef + ")";
    }
    return out;
}

// ─────────────────────────────────────────────
//  Equation solving
// ─────────────────────────────────────────────
// Substitute var with "(" value ")" and evaluate
static double subEval(const String& s, char var, double val) {
    char vb[24];
    snprintf(vb, sizeof(vb), "(%.10g)", val);
    String rep = vb;
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
    return evalCalc(result);
}

static double subEval2(const String& s, char v1, double a, char v2, double b) {
    // Substitute two variables (used by the system solver)
    char buf[64];
    String result = "";
    for (int i = 0; i < (int)s.length(); i++) {
        char c = s[i];
        double val = NAN;
        if (tolower(c) == v1) val = a;
        else if (tolower(c) == v2) val = b;
        if (!isnan(val)) {
            snprintf(buf, sizeof(buf), "(%.10g)", val);
            if (result.length() > 0) {
                char prev = result[result.length() - 1];
                if (isdigit(prev) || prev == ')') result += '*';
            }
            result += buf;
            if (i + 1 < (int)s.length()) {
                char next = s[i + 1];
                if (isdigit(next) || next == '(') result += '*';
            }
        } else {
            result += c;
        }
    }
    return evalCalc(result);
}

// Known words (not variables)
static bool isKnownWord(const String& w) {
    static const char* words[] = {
        "pi", "e", "ans", "sin", "cos", "tan", "rad", "sqrt",
        "log", "ln", "abs", "floor", "ceil", "round",
        "gcd", "lcm", "factor", nullptr
    };
    for (int i = 0; words[i]; i++)
        if (w == words[i]) return true;
    return false;
}

// Collect distinct single-letter variables. Multi-letter unknown runs
// (like "pix" or "xy") are skipped — evaluation will fail on them later.
static int findVars(const String& s, char* out, int maxOut) {
    int n = 0;
    int len = s.length();
    int i = 0;
    while (i < len) {
        char c = s[i];
        if (isalpha(c)) {
            int j = i;
            while (j < len && isalpha(s[j])) j++;
            String run = s.substring(i, j);
            run.toLowerCase();
            if (isKnownWord(run)) { i = j; continue; }
            if (run.length() == 1) {
                char v = run[0];
                bool seen = false;
                for (int k = 0; k < n; k++) if (out[k] == v) seen = true;
                if (!seen) {
                    if (n < maxOut) out[n++] = v;
                    else return maxOut + 1;   // too many variables
                }
            }
            i = j;
        } else {
            i++;
        }
    }
    return n;
}

static inline double relTol(double ref) { return 1e-9 * (1.0 + fabs(ref)); }

// f(v) = lhs(v) - rhs(v)
static double eqFunc(const String& lhs, const String& rhs, char var, double v) {
    double a = subEval(lhs, var, v);
    double b = subEval(rhs, var, v);
    if (isnan(a) || isnan(b)) return NAN;
    return a - b;
}

// Grid scan for a sign change + bisection; secant as fallback.
// (Plain secant diverges on things like x^x = 10.)
static double solveNumeric(const String& lhs, const String& rhs, char var) {
    static const double grid[] = {
        0, 0.25, 0.5, 1, 1.5, 2, 3, 4, 6, 8, 12, 16, 24, 32, 48, 64, 100,
        -0.25, -0.5, -1, -1.5, -2, -3, -4, -6, -8, -12, -16, -24, -32, -48, -64, -100
    };
    const int GN = sizeof(grid) / sizeof(grid[0]);

    double prevX = grid[0];
    double prevY = eqFunc(lhs, rhs, var, prevX);
    if (!isnan(prevY) && fabs(prevY) < 1e-10) return prevX;

    for (int i = 1; i < GN; i++) {
        double x = grid[i];
        double y = eqFunc(lhs, rhs, var, x);
        if (isnan(y)) { prevX = x; prevY = y; continue; }
        if (fabs(y) < 1e-10) return x;
        if (!isnan(prevY) && (prevY < 0) != (y < 0)) {
            // Bisection on [prevX, x]
            double lo = prevX, hi = x;
            double ylo = prevY;
            for (int it = 0; it < 80; it++) {
                double mid = (lo + hi) / 2.0;
                double ym = eqFunc(lhs, rhs, var, mid);
                if (isnan(ym)) break;
                if (fabs(ym) < 1e-10) return mid;
                if ((ylo < 0) == (ym < 0)) { lo = mid; ylo = ym; }
                else                       { hi = mid; }
                if (fabs(hi - lo) < 1e-12 * (1.0 + fabs(mid))) return mid;
            }
            return (lo + hi) / 2.0;
        }
        prevX = x; prevY = y;
    }

    // Secant fallback (no bracket found — e.g. tangent roots)
    static const double starts[] = { 0, 1, -1, 2, -2, 5, -5, 10, -10, 100, -100 };
    for (unsigned si = 0; si < sizeof(starts) / sizeof(starts[0]); si++) {
        double x0 = starts[si], x1 = starts[si] + 0.5;
        double y0 = eqFunc(lhs, rhs, var, x0);
        for (int it = 0; it < 80; it++) {
            double y1 = eqFunc(lhs, rhs, var, x1);
            if (isnan(y0) || isnan(y1)) break;
            if (fabs(y1) < 1e-10) return x1;
            double d = y1 - y0;
            if (fabs(d) < 1e-300) break;
            double x2 = x1 - y1 * (x1 - x0) / d;
            x0 = x1; y0 = y1; x1 = x2;
            if (fabs(x1) > 1e150) break;
        }
    }
    return NAN;
}

// Numeric fallback path (also used when probes hit domain gaps, e.g. log(x)=2)
static String solveEquationNumeric(const String& lhs, const String& rhs, char var) {
    String vs = String(var);
    double root = solveNumeric(lhs, rhs, var);
    if (isnan(root)) return "no real root found";
    double check = eqFunc(lhs, rhs, var, root);
    if (isnan(check) || fabs(check) > 1e-6 * (1.0 + fabs(root)))
        return "no real root found";
    lastAns = root; hasAns = true;
    return vs + " =~ " + fmtVal(root) + " (num)";
}

// Solve lhs = rhs for var. Handles linear, quadratic, numeric fallback.
static String solveEquation(const String& lhs, const String& rhs, char var) {
    double f0 = eqFunc(lhs, rhs, var, 0.0);
    double f1 = eqFunc(lhs, rhs, var, 1.0);
    double f2 = eqFunc(lhs, rhs, var, 2.0);
    if (isnan(f0) || isnan(f1) || isnan(f2)) {
        // Domain gaps (e.g. log(x) at x=0) → straight to numeric.
        // If every probe is NaN the expression is simply malformed.
        double f3t = eqFunc(lhs, rhs, var, 3.0);
        if (isnan(f0) && isnan(f1) && isnan(f2) && isnan(f3t)) return "error";
        return solveEquationNumeric(lhs, rhs, var);
    }

    String vs = String(var);

    // ── Linear?  f(v) = a*v + b ──
    double d1 = f1 - f0, d2 = f2 - f1;
    if (fabs(d2 - d1) < relTol(f2)) {
        double a = d1, b = f0;
        if (fabs(a) < 1e-12)
            return fabs(b) < relTol(b) ? "any value" : "no solution";
        double res = -b / a;
        lastAns = res; hasAns = true;
        if (fabs(res - round(res)) < 1e-9 && fabs(res) < 1e12)
            return vs + " = " + fmtVal(round(res));
        return vs + " =~ " + fmtVal(res);
    }

    // ── Quadratic?  f(v) = a2*v^2 + a1*v + a0 ──
    double a2 = (f2 - 2.0 * f1 + f0) / 2.0;
    double a1 = f1 - f0 - a2;
    double a0 = f0;
    double f3 = eqFunc(lhs, rhs, var, 3.0);
    bool isQuad = !isnan(f3) &&
                  fabs(f3 - (a0 + 3.0 * a1 + 9.0 * a2)) < relTol(f3);

    if (isQuad) {
        double disc = a1 * a1 - 4.0 * a2 * a0;
        if (disc < -relTol(a1 * a1)) return "no real roots";
        if (disc < 0) disc = 0;
        double sq = sqrt(disc);
        double r1 = (-a1 + sq) / (2.0 * a2);
        double r2 = (-a1 - sq) / (2.0 * a2);
        lastAns = r1; hasAns = true;
        if (fabs(r1 - r2) < relTol(r1))
            return vs + " = " + fmtVal(r1);
        return vs + " = " + fmtVal(r1) + "  or  " + fmtVal(r2);
    }

    // ── Nonlinear → numeric ──
    return solveEquationNumeric(lhs, rhs, var);
}

// Solve a system of 2 linear equations (separator: ',' at paren depth 0)
static String solveSystem(const String& e1, const String& e2) {
    int i1 = e1.indexOf('='), i2 = e2.indexOf('=');
    if (i1 < 0 || i2 < 0) return "error";
    String l1 = e1.substring(0, i1), r1 = e1.substring(i1 + 1);
    String l2 = e2.substring(0, i2), r2 = e2.substring(i2 + 1);
    l1.trim(); r1.trim(); l2.trim(); r2.trim();

    char vars[3];
    int nv = findVars(l1 + " " + r1 + " " + l2 + " " + r2, vars, 2);
    if (nv != 2) return "error";
    char vx = vars[0], vy = vars[1];

    // A*x + B*y = C for each equation (C = -f(0,0))
    double c1 = subEval2(l1, vx, 0, vy, 0) - subEval2(r1, vx, 0, vy, 0);
    double a1 = subEval2(l1, vx, 1, vy, 0) - subEval2(r1, vx, 1, vy, 0) - c1;
    double b1 = subEval2(l1, vx, 0, vy, 1) - subEval2(r1, vx, 0, vy, 1) - c1;
    double c2 = subEval2(l2, vx, 0, vy, 0) - subEval2(r2, vx, 0, vy, 0);
    double a2 = subEval2(l2, vx, 1, vy, 0) - subEval2(r2, vx, 1, vy, 0) - c2;
    double b2 = subEval2(l2, vx, 0, vy, 1) - subEval2(r2, vx, 0, vy, 1) - c2;
    if (isnan(c1) || isnan(a1) || isnan(b1) || isnan(c2) || isnan(a2) || isnan(b2))
        return "error";

    // Linearity check: f(2,0) = c + 2A, f(0,2) = c + 2B
    double t;
    t = subEval2(l1, vx, 2, vy, 0) - subEval2(r1, vx, 2, vy, 0);
    if (isnan(t) || fabs(t - (c1 + 2 * a1)) > relTol(t)) return "nonlinear system";
    t = subEval2(l1, vx, 0, vy, 2) - subEval2(r1, vx, 0, vy, 2);
    if (isnan(t) || fabs(t - (c1 + 2 * b1)) > relTol(t)) return "nonlinear system";
    t = subEval2(l2, vx, 2, vy, 0) - subEval2(r2, vx, 2, vy, 0);
    if (isnan(t) || fabs(t - (c2 + 2 * a2)) > relTol(t)) return "nonlinear system";
    t = subEval2(l2, vx, 0, vy, 2) - subEval2(r2, vx, 0, vy, 2);
    if (isnan(t) || fabs(t - (c2 + 2 * b2)) > relTol(t)) return "nonlinear system";

    // A*x + B*y = -c
    double det = a1 * b2 - a2 * b1;
    if (fabs(det) < 1e-12) return "no unique solution";
    double r1v = -c1, r2v = -c2;
    double xs = (r1v * b2 - r2v * b1) / det;
    double ys = (a1 * r2v - a2 * r1v) / det;
    lastAns = xs; hasAns = true;
    return String(vx) + "=" + fmtVal(xs) + " " + String(vy) + "=" + fmtVal(ys);
}

// Factor(n) — prime factorisation, string result
static String factorString(long long n) {
    if (n == 0) return "0";
    String out = "";
    if (n < 0) { out = "-"; n = -n; }
    bool first = true;
    for (long long p = 2; p * p <= n; p++) {
        if (n % p == 0) {
            int cnt = 0;
            while (n % p == 0) { n /= p; cnt++; }
            if (!first) out += '*';
            out += String((long)p);
            if (cnt > 1) { out += '^'; out += String(cnt); }
            first = false;
        }
    }
    if (n > 1) {
        if (!first) out += '*';
        out += String((long)n);
    }
    return out.length() ? out : "1";
}

// ─────────────────────────────────────────────
//  Main solver
// ─────────────────────────────────────────────
String solveExpression(String expr) {
    expr.replace(":", "/");
    expr.trim();

    // ── Factor(n) special form (string result) ──
    {
        String low = expr;
        low.toLowerCase();
        if (low.startsWith("factor(") && expr.endsWith(")")) {
            String inner = expr.substring(7, expr.length() - 1);
            double v = evalCalc(inner);
            if (isnan(v)) return "error";
            long long n = llround(v);
            if (fabs(v - n) > 1e-9 || fabs(v) > 1e9) return "error";
            return "= " + factorString(n);
        }
    }

    // ── System of equations: top-level ',' separator ──
    {
        int depth = 0, commaAt = -1;
        for (int i = 0; i < (int)expr.length(); i++) {
            char c = expr[i];
            if (c == '(') depth++;
            else if (c == ')') depth--;
            else if (c == ',' && depth == 0) {
                if (commaAt >= 0) return "error";   // only 2 equations
                commaAt = i;
            }
        }
        if (commaAt >= 0) {
            String e1 = expr.substring(0, commaAt);
            String e2 = expr.substring(commaAt + 1);
            e1.trim(); e2.trim();
            return solveSystem(e1, e2);
        }
    }

    int eqIdx = expr.indexOf('=');
    if (eqIdx >= 0) {
        String lhs = expr.substring(0, eqIdx);
        String rhs = expr.substring(eqIdx + 1);
        lhs.trim(); rhs.trim();

        char vars[3];
        // Separator is required — "3*Pi" + "x" would merge into run "Pix"
        int nv = findVars(lhs + " " + rhs, vars, 2);

        if (nv == 1) {
            return solveEquation(lhs, rhs, vars[0]);
        } else if (nv == 0) {
            // Identity check
            double lv = evalCalc(lhs);
            double rv = evalCalc(rhs);
            if (isnan(lv) || isnan(rv)) return "error";
            if (isinf(lv) || isinf(rv))
                return (lv == rv) ? "True" : "False";   // inf == inf
            return fabs(lv - rv) < relTol(lv) + relTol(rv) ? "True" : "False";
        } else {
            return "error";   // 2+ variables need the system form (',')
        }
    }

    // ── Plain expression ──
    double v = evalCalc(expr);
    if (isnan(v)) {
        if (g_evalErrPos >= 0 && g_evalErrPos < (int)expr.length())
            return "error @" + String(g_evalErrPos + 1);
        return "error";
    }
    lastAns = v; hasAns = true;
    return fmtNumber(v);
}

// ─────────────────────────────────────────────
//  SD history log
// ─────────────────────────────────────────────
void sdLog(const String& e, const String& res) {
    if (!sdOk) return;
    File f = SD.open(SD_HIST, FILE_APPEND);
    if (f) {
        f.println(e + "  ->  " + res);
        f.close();
    }
}

void addHistory(const String& e, const String& res) {
    if (histLen == HIST_MAX) {
        for (int i = 0; i < HIST_MAX - 1; i++) {
            histExpr[i] = histExpr[i + 1];
            histRes[i]  = histRes[i + 1];
        }
        histLen--;
    }
    histExpr[histLen] = e;
    histRes[histLen]  = res;
    histLen++;
    sdLog(e, res);
}

// ─────────────────────────────────────────────
//  Drawing helpers
// ─────────────────────────────────────────────
void drawTitle() {
    auto& d = M5Cardputer.Display;
    d.fillRect(0, 0, SCR_W, 18, C_BAR);
    d.setTextColor(C_WHITE);
    d.setTextSize(1);
    // Font0 has a predictable 6 px character width.  The former 9 pt title
    // could run past the 240 px display on some M5GFX font builds.
    d.setFont(&fonts::Font0);
    d.setTextDatum(TL_DATUM);
    d.drawString("OPT: help | ADV CALC " FW_VERSION, 3, 5);
    char battery[8];
    if (batteryLevel >= 0 && batteryLevel <= 100)
        snprintf(battery, sizeof(battery), "%d%%", batteryLevel);
    else
        snprintf(battery, sizeof(battery), "--");
    d.setTextColor(C_GRAY);
    d.setTextDatum(TR_DATUM);
    d.drawString(battery, SCR_W - 3, 5);
    d.setTextDatum(TL_DATUM);
}

// Redraw everything without touching state (result/history stay)
void drawBase() {
    auto& d = M5Cardputer.Display;
    d.fillScreen(C_BG);
    drawTitle();
    d.fillRect(0, Y_INFO, SCR_W, SCR_H - Y_INFO, C_BAR);
    d.setTextColor(C_WHITE);
    d.setTextSize(1);
    d.setFont(&fonts::Font0);
    d.setTextDatum(TL_DATUM);
    d.drawString("OK solve | hold DEL", 3, Y_INFO + 5);
    drawStatus();
    updateExprLine();
}

void drawInterface() {
    resultStr  = "";
    histBrowse = -1;
    drawBase();
}

void drawHelp() {
    auto& d = M5Cardputer.Display;
    d.fillScreen(C_BAR);
    d.setTextColor(C_WHITE);
    d.setFont(&fonts::FreeSansBold12pt7b);
    d.drawString("HELP", 95, 3);
    d.setFont(&fonts::FreeSans9pt7b);
    d.drawString(", / : cursor left/right",    6, 26);
    d.drawString("; . : history up/down",     6, 41);
    d.drawString("Caps+L/R/D: , / . chars",   6, 56);
    d.drawString("Fn: menu, L/R: tabs",       6, 71);
    d.drawString("OK: solve  DEL: hold=fast", 6, 86);
    d.drawString("Opt: close help",           6, 101);
}

void drawDropdown() {
    auto& d = M5Cardputer.Display;
    MenuTab& t = menuTabs[tabIdx];
    // Keep the popup below the title and above the info bar.  This is tall
    // enough for Math (the longest tab) without clipping its final row.
    const int x = 128, y = 19, w = 108;
    const int h = 18 + t.count * 11 + 4;
    d.fillRect(x, y, w, h, C_BAR);
    d.drawRect(x, y, w, h, C_WHITE);
    d.setFont(&fonts::FreeSans9pt7b);
    // Tab title + position indicator
    d.setTextColor(C_GREEN);
    char tt[24];
    snprintf(tt, sizeof(tt), "<%s %d/%d>", t.title, tabIdx + 1, TAB_COUNT);
    d.drawString(tt, x + 5, y + 4);
    for (int i = 0; i < t.count; i++) {
        d.setTextColor(i == mIdx ? C_GREEN : C_WHITE);
        if (t.items[i].isAction) {
            // Mode toggle shows current state
            d.drawString(degMode ? "Mode: DEG" : "Mode: RAD", x + 5, y + 18 + i * 11);
        } else {
            d.drawString(t.items[i].label, x + 5, y + 18 + i * 11);
        }
    }
}

// A menu is an overlay, so every change starts from a clean base.  Drawing
// just the next tab left pixels from taller tabs behind shorter ones.
void redrawMenu() {
    drawBase();
    drawDropdown();
}

void drawStatus() {
    auto& d = M5Cardputer.Display;
    char status[16];
    snprintf(status, sizeof(status), "%s", degMode ? "DEG" : "RAD");

    // The title and history rows stay dedicated to expressions.  Mode and
    // battery status belong with the keyboard bindings in the lower bar.
    d.fillRect(164, Y_INFO + 1, SCR_W - 164, SCR_H - Y_INFO - 1, C_BAR);
    d.setTextColor(C_GRAY);
    d.setTextSize(1);
    d.setFont(&fonts::Font0);
    d.setTextDatum(TR_DATUM);
    d.drawString(status, SCR_W - 3, Y_INFO + 5);
    d.setTextDatum(TL_DATUM);
}

void updateExprLine() {
    bool browsing = (histBrowse >= 0 && histLen > 0);

    canvas.fillSprite(C_BG);
    canvas.setTextSize(1);

    // ── Top row: previous expression, once a new expression is being typed ──
    canvas.setFont(&fonts::FreeSans9pt7b);
    int hi = histLen - 1 - (browsing ? histBrowse + 1 : 0);
    if ((browsing && histLen > 0 && hi >= 0) || showPreviousExpr) {
        // The previous *expression* is a small left-aligned context cue.
        // Keeping its result below the separator avoids colliding with the
        // right-aligned mode/battery text.
        String h = browsing ? histExpr[hi] : previousExpr;
        if (h.length() > 20) h = h.substring(0, 19) + ".";
        canvas.setTextColor(C_GRAY);
        canvas.setTextDatum(TL_DATUM);
        canvas.drawString(h, 3, Y_HIST - CANVAS_Y);
    }
    canvas.setTextDatum(TL_DATUM);

    // ── Expression (horizontal scroll window around cursor) ──
    if (cursorPos < winStart) winStart = cursorPos;
    if (cursorPos > winStart + MAXVIS - 1) winStart = cursorPos - MAXVIS + 1;
    int endPos = winStart + MAXVIS;
    if (endPos > (int)eq.length()) endPos = eq.length();

    String visible = eq.substring(winStart, cursorPos)
                   + (cursorOn ? "|" : " ")
                   + eq.substring(cursorPos, endPos);

    canvas.setTextColor(browsing ? C_GRAY : C_WHITE);
    canvas.setFont(&fonts::FreeSansBold12pt7b);
    String prefix = (winStart > 0) ? "<" : "";
    String suffix = (endPos < (int)eq.length()) ? ">" : "";
    canvas.drawString(prefix + visible + suffix, 5, Y_EXPR - CANVAS_Y);

    // ── Result ──
    String res = browsing ? histRes[histLen - 1 - histBrowse] : resultStr;
    if (res.length() > 0) {
        canvas.fillRect(0, Y_RESULT - CANVAS_Y + 4, 5, 16, C_WHITE);
        canvas.setTextColor(browsing ? C_GRAY : C_WHITE);
        canvas.drawString(res, 12, Y_RESULT - CANVAS_Y);
    }

    // This must be inside the sprite: a line drawn by drawBase would be
    // overwritten by the sprite's background on the very next input redraw.
    canvas.drawLine(1, Y_SEP - CANVAS_Y, SCR_W - 1, Y_SEP - CANVAS_Y, C_LINE);

    canvas.pushSprite(0, CANVAS_Y);
}

// ─────────────────────────────────────────────
//  Input helpers
// ─────────────────────────────────────────────
void insertText(const String& s, int cursorOffset) {
    if (histBrowse >= 0) histBrowse = -1;   // browsing → becomes new edit
    // Keep the prior completed expression visible while editing the next one.
    // It is replaced only after the next successful Enter.
    eq = eq.substring(0, cursorPos) + s + eq.substring(cursorPos);
    cursorPos += cursorOffset;
    resultStr = "";
    updateExprLine();
}

void doBackspace() {
    if (histBrowse >= 0) histBrowse = -1;
    if (cursorPos > 0) {
        eq = eq.substring(0, cursorPos - 1) + eq.substring(cursorPos);
        cursorPos--;
        resultStr = "";
    }
    updateExprLine();
}

// ─────────────────────────────────────────────
//  setup / loop
// ─────────────────────────────────────────────
void setup() {
    auto cfg = M5.config();
    M5Cardputer.begin(cfg, true);
    M5Cardputer.Display.setRotation(1);
    M5Cardputer.Display.setBrightness(BRIGHTNESS);
    canvas.setColorDepth(16);
    canvas.createSprite(SCR_W, CANVAS_H);
    batteryLevel = M5Cardputer.Power.getBatteryLevel();

    // SD card (optional — silently skip if absent)
    SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
    if (SD.begin(SD_CS, SPI, 25000000)) {
        sdOk = true;
        if (!SD.exists(SD_DIR)) SD.mkdir(SD_DIR);
    }

    lastActivity = millis();
    drawInterface();
}

bool prevFn  = false;
bool prevOpt = false;

void loop() {
    M5Cardputer.update();
    unsigned long now = millis();

    // ── Autosleep ──
    if (!sleeping && now - lastActivity > IDLE_MS) {
        sleeping = true;
        M5Cardputer.Display.setBrightness(0);
    }

    // ── DEL auto-repeat (hold = fast erase) ──
    if (delHeld && !sleeping && !menuActive && !helpActive) {
        if (now - delStart > DEL_DELAY_MS && now - delRepeat > DEL_REPEAT_MS) {
            delRepeat = now;
            doBackspace();
        }
    }

    // ── Cursor blink ──
    if (now - lastBlink >= BLINK_MS) {
        lastBlink = now;
        cursorOn  = !cursorOn;
        if (!menuActive && !helpActive && !sleeping) updateExprLine();
    }

    // ── Battery refresh (every 30 s) ──
    if (!sleeping && !menuActive && !helpActive && now - lastBattCheck > 30000) {
        lastBattCheck = now;
        batteryLevel = M5Cardputer.Power.getBatteryLevel();
        drawTitle();
        drawStatus();
        updateExprLine();   // mode/battery live in the canvas top row
    }

    if (!M5Cardputer.Keyboard.isChange()) return;
    lastActivity = now;

    // Wake from sleep: consume this keypress.
    // Screen was only dimmed — content is intact, just restore brightness.
    if (sleeping) {
        sleeping = false;
        M5Cardputer.Display.setBrightness(BRIGHTNESS);
        return;
    }

    if (!M5Cardputer.Keyboard.isPressed()) {
        // All keys released
        prevFn  = false;
        prevOpt = false;
        delHeld = false;
        return;
    }

    Keyboard_Class::KeysState s = M5Cardputer.Keyboard.keysState();

    // ── Opt key: toggle help ─────────────────────────────────
    if (s.opt && !prevOpt) {
        helpActive = !helpActive;
        menuActive = false;
        if (helpActive) drawHelp();
        else            drawBase();
        prevOpt = s.opt;
        delHeld = false;
        return;
    }
    prevOpt = s.opt;

    if (helpActive) { prevFn = s.fn; delHeld = false; return; }

    // ── Fn layer: menu toggle ──────────────────────────────
    if (s.fn) {
        if (!prevFn) {
            menuActive = !menuActive;
            if (menuActive) { redrawMenu(); }
            else            { drawBase(); }
        }
        prevFn = true;
        delHeld = false;
        return;
    }
    prevFn = false;

    // ── Menu navigation ─────────────────────────────────────
    if (menuActive) {
        delHeld = false;
        if (s.enter) {
            MenuTab& t = menuTabs[tabIdx];
            MenuItem& it = t.items[mIdx];
            if (it.isAction) {
                degMode = !degMode;
                redrawMenu();     // also refreshes the mode text in the base
            } else {
                String ins = String(it.insert);
                if (it.hasParen) {
                    ins += ")";
                    menuActive = false;
                    drawBase();
                    insertText(ins, ins.length() - 1);
                } else {
                    menuActive = false;
                    drawBase();
                    insertText(ins, ins.length());
                }
            }
        } else {
            for (auto c : s.word) {
                if (c == ';') {          // up
                    mIdx = (mIdx - 1 + menuTabs[tabIdx].count) % menuTabs[tabIdx].count;
                    redrawMenu();
                } else if (c == '.') {   // down
                    mIdx = (mIdx + 1) % menuTabs[tabIdx].count;
                    redrawMenu();
                } else if (c == ',') {   // left → prev tab
                    tabIdx = (tabIdx - 1 + TAB_COUNT) % TAB_COUNT;
                    mIdx = 0;
                    redrawMenu();
                } else if (c == '/') {   // right → next tab
                    tabIdx = (tabIdx + 1) % TAB_COUNT;
                    mIdx = 0;
                    redrawMenu();
                }
            }
        }
        return;
    }

    // ── Normal input ────────────────────────────────────────

    // Backspace (starts auto-repeat)
    if (s.del) {
        delHeld   = true;
        delStart  = now;
        delRepeat = now;
        doBackspace();
        return;
    }
    delHeld = false;

    // Enter → solve
    if (s.enter) {
        if (eq.length() > 0) {
            String lastCompleted = histLen > 0 ? histExpr[histLen - 1] : "";
            resultStr = solveExpression(eq);
            addHistory(eq, resultStr);
            histBrowse = -1;
            previousExpr = lastCompleted;
            showPreviousExpr = previousExpr.length() > 0;
        }
        updateExprLine();
        return;
    }

    // Printable characters
    for (auto c : s.word) {
        // Arrow key mappings (physical keys produce these chars):
        //   ';' = up    → history back
        //   '.' = down  → history forward
        //   ',' = left  → cursor left
        //   '/' = right → cursor right
        //   '<' = caps+left  → insert ','
        //   '?' = caps+right → insert '/'
        //   '>' = caps+down  → insert '.'
        if (c == ',') {
            if (cursorPos > 0) cursorPos--;
            updateExprLine();
            continue;
        }
        if (c == '/') {
            if (cursorPos < (int)eq.length()) cursorPos++;
            updateExprLine();
            continue;
        }
        if (c == ';') {   // history: older
            if (histLen > 0) {
                if (histBrowse < 0) { editBuf = eq; histBrowse = 0; }
                else if (histBrowse < histLen - 1) histBrowse++;
                eq = histExpr[histLen - 1 - histBrowse];
                cursorPos = eq.length();
            }
            updateExprLine();
            continue;
        }
        if (c == '.') {   // history: newer / back to edit
            if (histBrowse > 0) {
                histBrowse--;
                eq = histExpr[histLen - 1 - histBrowse];
                cursorPos = eq.length();
            } else if (histBrowse == 0) {
                histBrowse = -1;
                eq = editBuf;
                cursorPos = eq.length();
            }
            updateExprLine();
            continue;
        }
        if (c == '<') { insertText(",", 1); continue; }
        if (c == '?') { insertText("/", 1); continue; }
        if (c == '>') { insertText(".", 1); continue; }

        // Auto-close parens
        if (c == '(') { insertText("()", 1); continue; }
        if (c == ')') {
            // Skip over an auto-inserted closing paren
            if (cursorPos < (int)eq.length() && eq[cursorPos] == ')') {
                cursorPos++;
                updateExprLine();
            } else {
                insertText(")", 1);
            }
            continue;
        }

        // Calculator-valid characters
        if (isdigit(c) || isalpha(c) ||
            c == '+' || c == '-' || c == '*' ||
            c == '=' || c == ' ' || c == '^' ||
            c == '%' || c == '!') {
            insertText(String(c), 1);
        }
    }
}
