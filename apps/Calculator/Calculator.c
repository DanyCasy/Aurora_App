#include <aurora_app.h>

#include <ctype.h>

#include <math.h>

#include <stdbool.h>

#include <stdio.h>

#include <stdlib.h>

#include <string.h>
#include <stdint.h>
#include <time.h>

/* ============================================================

   Calculator3DS

   Scientific calculator for AuroraOS / Nintendo 3DS

   ============================================================ */

#define EXPR_SIZE       256

#define RESULT_SIZE     80

#define ERROR_SIZE      100

#define HISTORY_SIZE    8

#define BASIC_KEY_COUNT 20
#define SCI_KEY_COUNT 30
#define KEY_COUNT SCI_KEY_COUNT
#define PI_VALUE 3.14159265358979323846
#define E_VALUE 2.71828182845904523536

/* iPhone Calculator-inspired: black canvas, gray keys, orange operators. */
#define C_BG            0x000000u
#define C_PANEL         0x000000u
#define C_PANEL_LIGHT   0x333333u
#define C_ACCENT        0xFFFFFFu
#define C_ACCENT_DARK   0xFF9F0Au
#define C_PURPLE        0xB0B0B0u
#define C_TEXT          0xFFFFFFu
#define C_TEXT_DARK     0x202020u
#define C_MUTED         0xAAAAAAu
#define C_OPERATOR      0xFF9F0Au
#define C_FUNCTION      0x505050u
#define C_DANGER        0xA5A5A5u
#define C_ERROR         0xFF6961u
#define C_BORDER       0xFFFFFFu

typedef struct {

    char expression[EXPR_SIZE];

    char result[RESULT_SIZE];

    char error[ERROR_SIZE];

    double ans;

    double memory;

    bool has_ans;

    bool has_memory;

    bool degrees;

    bool second_mode;

    bool scientific_page;

    bool show_history;

    bool show_help;

    int selected_key;

    int history_count;

    int history_position;

    char history_expr[HISTORY_SIZE][EXPR_SIZE];

    char history_result[HISTORY_SIZE][RESULT_SIZE];

} Calculator;

static Calculator calc;

/* ============================================================

   Keypad

   ============================================================ */

typedef struct {

    const char *label;

    const char *insert;

    unsigned int color;

} Key;

static const Key basic_keypad[BASIC_KEY_COUNT] = {
    { "AC", "AC", C_DANGER }, { "+/-", "SIGN", C_DANGER }, { "%", "%", C_DANGER }, { "/", "/", C_OPERATOR },
    { "7", "7", C_PANEL_LIGHT }, { "8", "8", C_PANEL_LIGHT }, { "9", "9", C_PANEL_LIGHT }, { "*", "*", C_OPERATOR },
    { "4", "4", C_PANEL_LIGHT }, { "5", "5", C_PANEL_LIGHT }, { "6", "6", C_PANEL_LIGHT }, { "-", "-", C_OPERATOR },
    { "1", "1", C_PANEL_LIGHT }, { "2", "2", C_PANEL_LIGHT }, { "3", "3", C_PANEL_LIGHT }, { "+", "+", C_OPERATOR },
    { "0", "0", C_PANEL_LIGHT }, { ".", ".", C_PANEL_LIGHT }, { "DEL", "DEL", C_DANGER }, { "=", "EQUAL", C_OPERATOR }
};

/* Scientific page mirrors the iPhone scientific keypad's 6 x 5 layout. */
static const Key scientific_keypad[SCI_KEY_COUNT] = {
    { "(", "(", C_FUNCTION }, { ")", ")", C_FUNCTION }, { "mc", "MC", C_FUNCTION }, { "m+", "M+", C_FUNCTION }, { "m-", "M-", C_FUNCTION }, { "mr", "MR", C_FUNCTION },
    { "2nd", "SECOND", C_FUNCTION }, { "x^2", "^2", C_FUNCTION }, { "x^3", "^3", C_FUNCTION }, { "x^y", "^", C_FUNCTION }, { "e^x", "exp(", C_FUNCTION }, { "10^x", "tenpow(", C_FUNCTION },
    { "1/x", "inv(", C_FUNCTION }, { "sqrt", "sqrt(", C_FUNCTION }, { "cbrt", "cbrt(", C_FUNCTION }, { "yroot", "^(1/", C_FUNCTION }, { "ln", "ln(", C_FUNCTION }, { "log10", "log(", C_FUNCTION },
    { "x!", "!", C_FUNCTION }, { "sin", "sin(", C_FUNCTION }, { "cos", "cos(", C_FUNCTION }, { "tan", "tan(", C_FUNCTION }, { "e", "e", C_FUNCTION }, { "EE", "*10^", C_FUNCTION },
    { "Rand", "rand()", C_FUNCTION }, { "sinh", "sinh(", C_FUNCTION }, { "cosh", "cosh(", C_FUNCTION }, { "tanh", "tanh(", C_FUNCTION }, { "pi", "pi", C_FUNCTION }, { "DEG", "RAD", C_FUNCTION }
};

static const Key *active_keypad(void) { return calc.scientific_page ? scientific_keypad : basic_keypad; }
static int active_key_count(void) { return calc.scientific_page ? SCI_KEY_COUNT : BASIC_KEY_COUNT; }
static int active_cols(void) { return calc.scientific_page ? 6 : 4; }
static int active_rows(void) { return calc.scientific_page ? 5 : 5; }


/* ============================================================

   Parser

   ============================================================ */

typedef struct {

    const char *p;

    Calculator *owner;

    bool failed;

    char error[ERROR_SIZE];

} Parser;

static void parser_fail(Parser *p, const char *message)

{

    if (p->failed)

        return;

    p->failed = true;

    snprintf(

        p->error,

        sizeof(p->error),

        "%s",

        message

    );

}

static void skip_spaces(Parser *p)

{

    while (*p->p && isspace((unsigned char)*p->p))

        p->p++;

}

static bool consume(Parser *p, char c)

{

    skip_spaces(p);

    if (*p->p != c)

        return false;

    p->p++;

    return true;

}

static bool finite_result(Parser *p, double value)

{

    if (!isfinite(value)) {

        parser_fail(p, "Result cannot be represented");

        return false;

    }

    return true;

}

static double to_radians(Calculator *c, double value)

{

    return c->degrees ? value * PI_VALUE / 180.0 : value;

}

static double from_radians(Calculator *c, double value)

{

    return c->degrees ? value * 180.0 / PI_VALUE : value;

}

static double parse_expression(Parser *p);

static double parse_unary(Parser *p);

/* ------------------------------------------------------------

   Functions

   \------------------------------------------------------------ */

static double apply_function(Parser *p, const char *name, double x)

{

    Calculator *c = p->owner;

    double result = 0.0;

    if (strcmp(name, "sin") == 0) {

        result = sin(to_radians(c, x));

    } else if (strcmp(name, "cos") == 0) {

        result = cos(to_radians(c, x));

    } else if (strcmp(name, "tan") == 0) {

        double angle = to_radians(c, x);

        if (fabs(cos(angle)) < 1e-12) {

            parser_fail(p, "Tangent is undefined");

            return 0.0;

        }

        result = tan(angle);

    } else if (strcmp(name, "asin") == 0) {

        if (x < -1.0 || x > 1.0) {

            parser_fail(p, "asin: value out of range");

            return 0.0;

        }

        result = from_radians(c, asin(x));

    } else if (strcmp(name, "acos") == 0) {

        if (x < -1.0 || x > 1.0) {

            parser_fail(p, "acos: value out of range");

            return 0.0;

        }

        result = from_radians(c, acos(x));

    } else if (strcmp(name, "atan") == 0) {

        result = from_radians(c, atan(x));

    } else if (strcmp(name, "sqrt") == 0) {

        if (x < 0.0) {

            parser_fail(p, "Negative square root");

            return 0.0;

        }

        result = sqrt(x);

    } else if (strcmp(name, "ln") == 0) {

        if (x <= 0.0) {

            parser_fail(p, "ln requires a positive value");

            return 0.0;

        }

        result = log(x);

    } else if (strcmp(name, "log") == 0) {

        if (x <= 0.0) {

            parser_fail(p, "log requires a positive value");

            return 0.0;

        }

        result = log10(x);

    } else if (strcmp(name, "exp") == 0) {

        result = exp(x);

    } else if (strcmp(name, "abs") == 0) {

        result = fabs(x);

    } else if (strcmp(name, "inv") == 0) {

        if (x == 0.0) {

            parser_fail(p, "Division by zero");

            return 0.0;

        }

        result = 1.0 / x;

    } else if (strcmp(name, "cbrt") == 0) {
        result = cbrt(x);
    } else if (strcmp(name, "sinh") == 0) {
        result = sinh(x);
    } else if (strcmp(name, "cosh") == 0) {
        result = cosh(x);
    } else if (strcmp(name, "tanh") == 0) {
        result = tanh(x);
    } else if (strcmp(name, "asinh") == 0) {
        result = asinh(x);
    } else if (strcmp(name, "acosh") == 0) {
        if (x < 1.0) { parser_fail(p, "acosh requires x >= 1"); return 0.0; }
        result = acosh(x);
    } else if (strcmp(name, "atanh") == 0) {
        if (x <= -1.0 || x >= 1.0) { parser_fail(p, "atanh requires -1 < x < 1"); return 0.0; }
        result = atanh(x);
    } else if (strcmp(name, "tenpow") == 0) {
        result = pow(10.0, x);
    } else if (strcmp(name, "rand") == 0) {
        result = (double)rand() / ((double)RAND_MAX + 1.0);
    } else {

        parser_fail(p, "Unknown function");

        return 0.0;

    }

    finite_result(p, result);

    return result;

}

/* ------------------------------------------------------------

   Identifiers and constants

   \------------------------------------------------------------ */

static double parse_identifier(Parser *p)

{

    char name[24];

    size_t n = 0;

    skip_spaces(p);

    while (isalpha((unsigned char)*p->p)) {

        if (n + 1 < sizeof(name)) {

            name[n++] =

                (char)tolower((unsigned char)*p->p);

        }

        p->p++;

    }

    name[n] = '\0';

    if (strcmp(name, "pi") == 0)

        return PI_VALUE;

    if (strcmp(name, "e") == 0)

        return E_VALUE;

    if (strcmp(name, "ans") == 0) {

        if (!p->owner->has_ans) {

            parser_fail(p, "No previous result");

            return 0.0;

        }

        return p->owner->ans;

    }

    if (strcmp(name, "mr") == 0) {

        if (!p->owner->has_memory) {

            parser_fail(p, "Memory is empty");

            return 0.0;

        }

        return p->owner->memory;

    }

    if (strcmp(name, "rand") == 0 && consume(p, '(')) {
        if (!consume(p, ')')) { parser_fail(p, "rand takes no arguments"); return 0.0; }
        return (double)rand() / ((double)RAND_MAX + 1.0);
    }
    if (strcmp(name, "root") == 0 && consume(p, '(')) {
        double degree = parse_expression(p);
        if (!consume(p, ',')) { parser_fail(p, "Use root(degree,value)"); return 0.0; }
        double radicand = parse_expression(p);
        if (!consume(p, ')')) { parser_fail(p, "Missing closing parenthesis"); return 0.0; }
        if (degree == 0.0) { parser_fail(p, "Root degree cannot be zero"); return 0.0; }
        if (radicand < 0.0) {
            if (!isfinite(degree) || fabs(degree) > 1000000.0) {
                parser_fail(p, "Root degree out of range");
                return 0.0;
            }
            double d = round(degree);
            if (fabs(degree - d) > 1e-10 || ((long)d % 2L) == 0) {
                parser_fail(p, "Even root of a negative number");
                return 0.0;
            }
            return -pow(-radicand, 1.0 / degree);
        }
        return pow(radicand, 1.0/degree);
    }
    if (consume(p, '(')) {

        double argument = parse_expression(p);

        if (!consume(p, ')')) {

            parser_fail(p, "Missing closing parenthesis");

            return 0.0;

        }

        return apply_function(p, name, argument);

    }

    parser_fail(p, "Unknown function or constant");

    return 0.0;

}

/* ------------------------------------------------------------

   Primary expressions

   \------------------------------------------------------------ */

static double parse_primary(Parser *p)

{

    skip_spaces(p);

    if (consume(p, '(')) {

        double value = parse_expression(p);

        if (!consume(p, ')'))

            parser_fail(p, "Missing closing parenthesis");

        return value;

    }

    if (isalpha((unsigned char)*p->p))

        return parse_identifier(p);

    char *end = NULL;

    double value = strtod(p->p, &end);

    if (end == p->p) {

        parser_fail(p, "Expected a number or parenthesis");

        return 0.0;

    }

    p->p = end;

    finite_result(p, value);

    return value;

}

/* ------------------------------------------------------------

   Postfix operators: factorial and percentage

   \------------------------------------------------------------ */

static double parse_postfix(Parser *p)

{

    double value = parse_primary(p);

    while (!p->failed) {

        skip_spaces(p);

        if (*p->p == '!') {

            p->p++;

            if (value < 0.0 || value > 170.0) {

                parser_fail(p, "Factorial out of range");

                return 0.0;

            }

            double rounded = floor(value + 0.5);

            if (fabs(value - rounded) > 1e-10) {

                parser_fail(p, "Factorial: an integer is required");

                return 0.0;

            }

            value = 1.0;

            for (int i = 2; i <= (int)rounded; i++)

                value *= (double)i;

        } else if (*p->p == '%') {

            p->p++;

            value /= 100.0;

        } else {

            break;

        }

        if (!finite_result(p, value))

            return 0.0;

    }

    return value;

}

/* ------------------------------------------------------------

   Power and unary operators

   -2^2  -> -4

   (-2)^2 -> 4

   2^3^2 -> 512

   2^-2  -> 0.25

   \------------------------------------------------------------ */

static double parse_power(Parser *p)

{

    double left = parse_postfix(p);

    if (consume(p, '^')) {

        double right = parse_unary(p);

        if (p->failed)

            return 0.0;

        left = pow(left, right);

        finite_result(p, left);

    }

    return left;

}

static double parse_unary(Parser *p)

{

    if (consume(p, '+'))

        return parse_unary(p);

    if (consume(p, '-'))

        return -parse_unary(p);

    return parse_power(p);

}

/* ------------------------------------------------------------

   Implicit multiplication helpers

   \------------------------------------------------------------ */

static bool begins_primary(Parser *p)

{

    skip_spaces(p);

    return *p->p == '(' ||

           *p->p == '.' ||

           isdigit((unsigned char)*p->p) ||

           isalpha((unsigned char)*p->p);

}

/* ------------------------------------------------------------

   Multiplication and division

   \------------------------------------------------------------ */

static double parse_product(Parser *p)

{

    double value = parse_unary(p);

    while (!p->failed) {

        skip_spaces(p);

        if (consume(p, '*')) {

            value *= parse_unary(p);

        } else if (consume(p, '/')) {

            double divisor = parse_unary(p);

            if (divisor == 0.0) {

                parser_fail(p, "Division by zero");

                return 0.0;

            }

            value /= divisor;

        } else if (begins_primary(p)) {

            /*

             * Implicit multiplication:

             * 2pi, 2(3+4), (2+3)(4+5), 3sin(30)

             */

            value *= parse_unary(p);

        } else {

            break;

        }

        if (!finite_result(p, value))

            return 0.0;

    }

    return value;

}

/* ------------------------------------------------------------

   Addition and subtraction

   \------------------------------------------------------------ */

static double parse_expression(Parser *p)

{

    double value = parse_product(p);

    while (!p->failed) {

        if (consume(p, '+')) {

            value += parse_product(p);

        } else if (consume(p, '-')) {

            value -= parse_product(p);

        } else {

            break;

        }

        if (!finite_result(p, value))

            return 0.0;

    }

    return value;

}

/* ------------------------------------------------------------

   Evaluate a complete expression

   \------------------------------------------------------------ */

static bool evaluate(

    Calculator *owner,

    const char *expression,

    double *out,

    char *error,

    size_t error_size)

{

    Parser p;

    memset(&p, 0, sizeof(p));

    p.p = expression;

    p.owner = owner;

    if (!expression || !*expression) {

        snprintf(error, error_size, "Enter an expression");

        return false;

    }

    double value = parse_expression(&p);

    skip_spaces(&p);

    if (!p.failed && *p.p != '\0')

        parser_fail(&p, "Invalid syntax");

    if (p.failed) {

        snprintf(error, error_size, "%s", p.error);

        return false;

    }

    if (!isfinite(value)) {

        snprintf(error, error_size, "Result cannot be represented");

        return false;

    }

    *out = value;

    error[0] = '\0';

    return true;

}

/* ============================================================

   Expression editing

   ============================================================ */

static size_t expression_length(void)

{

    return strlen(calc.expression);

}

static void append_text(const char *text)

{

    size_t current = expression_length();

    size_t add = strlen(text);

    if (current + add >= sizeof(calc.expression)) {

        snprintf(

            calc.error,

            sizeof(calc.error),

            "Expression too long"

        );

        return;

    }

    memcpy(

        calc.expression + current,

        text,

        add + 1

    );

    calc.error[0] = '\0';
    calc.result[0] = '\0';

    calc.show_history = false;

    calc.show_help = false;

}

static void append_char(char c)

{

    char text[2];

    text[0] = c;

    text[1] = '\0';

    append_text(text);

}

static void delete_last(void)
{
    size_t n = expression_length();
    if (n > 0) {
        static const char *tokens[] = {
            "asinh(", "acosh(", "atanh(", "tenpow(", "sqrt(", "cbrt(",
            "root(", "sinh(", "cosh(", "tanh(", "asin(", "acos(",
            "atan(", "sin(", "cos(", "tan(", "inv(", "exp(",
            "ln(", "log(", "rand("
        };
        bool removed_token = false;
        for (size_t i=0; i<sizeof(tokens)/sizeof(tokens[0]); i++) {
            size_t t = strlen(tokens[i]);
            if (n >= t && strcmp(calc.expression+n-t, tokens[i]) == 0) {
                calc.expression[n-t] = '\0';
                removed_token = true;
                break;
            }
        }
        if (!removed_token) calc.expression[n-1] = '\0';
    }
    calc.error[0] = '\0';
    calc.result[0] = '\0';
}

static void clear_expression(void)

{

    calc.expression[0] = '\0';

    calc.result[0] = '\0';

    calc.error[0] = '\0';

}

/*

 * Toggles the sign of the entire expression.

 * This is intentionally different from editing the last operand.

 */

static bool is_unary_boundary(char ch)
{
    return ch == '(' || ch == '+' || ch == '-' || ch == '*' ||
           ch == '/' || ch == '^' || ch == ',';
}

static void toggle_sign(void)
{
    size_t n = expression_length();
    size_t start = n;
    if (n == 0) {
        append_char('-');
        return;
    }

    /* Find the last atom: a number, a constant/function call, or a group. */
    if (calc.expression[n - 1] == ')') {
        size_t depth = 0;
        size_t open = n;
        do {
            char ch = calc.expression[--open];
            if (ch == ')') depth++;
            else if (ch == '(') depth--;
        } while (open > 0 && depth > 0);
        if (depth == 0) {
            start = open;
            /* Include a function name immediately before its opening parenthesis. */
            while (start > 0 && isalpha((unsigned char)calc.expression[start - 1])) start--;
        }
    } else if (isdigit((unsigned char)calc.expression[n - 1]) || calc.expression[n - 1] == '.') {
        start = n;
        while (start > 0 &&
               (isdigit((unsigned char)calc.expression[start - 1]) || calc.expression[start - 1] == '.')) start--;
    } else if (isalpha((unsigned char)calc.expression[n - 1])) {
        start = n;
        while (start > 0 && isalpha((unsigned char)calc.expression[start - 1])) start--;
    } else if (calc.expression[n - 1] == '!' || calc.expression[n - 1] == '%') {
        /* Treat postfix operations as an atom by negating the expression before them. */
        char postfix = calc.expression[n - 1];
        calc.expression[n - 1] = '\0';
        toggle_sign();
        size_t m = strlen(calc.expression);
        if (m + 1 < sizeof(calc.expression)) {
            calc.expression[m] = postfix;
            calc.expression[m + 1] = '\0';
        }
        calc.error[0] = '\0';
        calc.result[0] = '\0';
        calc.show_history = false;
        calc.show_help = false;
        return;
    } else {
        /* When the entry ends with an operator, start a unary negative operand. */
        char last = calc.expression[n - 1];
        if (is_unary_boundary(last)) append_char('-');
        else append_text("*(-1)");
        calc.error[0] = '\0';
        calc.result[0] = '\0';
        calc.show_history = false;
        calc.show_help = false;
        return;
    }

    if (start == n) {
        /* No clear trailing atom was found (for example a just-entered operator). */
        append_text("*(-1)");
    } else {
        bool already_negative = start > 0 && calc.expression[start - 1] == '-' &&
            (start == 1 || is_unary_boundary(calc.expression[start - 2]));
        if (already_negative) {
            memmove(calc.expression + start - 1, calc.expression + start, n - start + 1);
        } else if (start < sizeof(calc.expression) - 2) {
            bool implicit_product = start > 0 &&
                (isdigit((unsigned char)calc.expression[start - 1]) ||
                 isalpha((unsigned char)calc.expression[start - 1]) ||
                 calc.expression[start - 1] == '.' ||
                 calc.expression[start - 1] == ')' ||
                 calc.expression[start - 1] == '!' ||
                 calc.expression[start - 1] == '%');
            if (implicit_product) {
                memmove(calc.expression + start + 2, calc.expression + start, n - start + 1);
                calc.expression[start] = '*';
                calc.expression[start + 1] = '-';
            } else {
                memmove(calc.expression + start + 1, calc.expression + start, n - start + 1);
                calc.expression[start] = '-';
            }
        }
    }
    calc.error[0] = '\0';
    calc.result[0] = '\0';
    calc.show_history = false;
    calc.show_help = false;
}

/* ============================================================

   History and result formatting

   ============================================================ */

static void format_number(double value, char *buffer, size_t size)

{

    if (fabs(value) < 1e-14)

        value = 0.0;

    snprintf(buffer, size, "%.12g", value);

}

static void add_history(const char *expression, const char *result)

{

    if (calc.history_count == HISTORY_SIZE) {

        for (int i = 1; i < HISTORY_SIZE; i++) {

            strcpy(

                calc.history_expr[i - 1],

                calc.history_expr[i]

            );

            strcpy(

                calc.history_result[i - 1],

                calc.history_result[i]

            );

        }

        calc.history_count--;

    }

    int i = calc.history_count++;

    snprintf(

        calc.history_expr[i],

        sizeof(calc.history_expr[i]),

        "%s",

        expression

    );

    snprintf(

        calc.history_result[i],

        sizeof(calc.history_result[i]),

        "%s",

        result

    );

    calc.history_position = calc.history_count - 1;

}

static void calculate(void)

{

    double value = 0.0;

    char error[ERROR_SIZE];

    char expression_copy[EXPR_SIZE];

    if (!calc.expression[0]) {

        snprintf(

            calc.error,

            sizeof(calc.error),

            "Enter an expression"

        );

        return;

    }

    snprintf(

        expression_copy,

        sizeof(expression_copy),

        "%s",

        calc.expression

    );

    if (!evaluate(

            &calc,

            expression_copy,

            &value,

            error,

            sizeof(error))) {

        snprintf(calc.error, sizeof(calc.error), "%s", error);

        snprintf(calc.result, sizeof(calc.result), "Errore");

        return;

    }

    format_number(value, calc.result, sizeof(calc.result));

    calc.ans = value;

    calc.has_ans = true;

    calc.error[0] = '\0';

    add_history(expression_copy, calc.result);

}

static void recall_ans(void)

{

    if (!calc.has_ans) {

        snprintf(

            calc.error,

            sizeof(calc.error),

            "No previous result"

        );

        return;

    }

    append_text("Ans");

}

static void recall_history(int direction)

{

    if (calc.history_count == 0)

        return;

    calc.history_position += direction;

    if (calc.history_position < 0)

        calc.history_position = 0;

    if (calc.history_position >= calc.history_count)

        calc.history_position = calc.history_count - 1;

    snprintf(

        calc.expression,

        sizeof(calc.expression),

        "%s",

        calc.history_expr[calc.history_position]

    );

    calc.result[0] = '\0';

    calc.error[0] = '\0';

    calc.show_history = false;

}

/* ============================================================

   Memory

   ============================================================ */

static bool current_value(double *value)

{

    char error[ERROR_SIZE];

    if (!evaluate(

            &calc,

            calc.expression,

            value,

            error,

            sizeof(error))) {

        snprintf(calc.error, sizeof(calc.error), "%s", error);

        return false;

    }

    return true;

}

static void memory_add(void)

{

    double value;

    if (!current_value(&value))

        return;

    calc.memory += value;

    if (!isfinite(calc.memory)) {

        calc.memory = 0.0;

        calc.has_memory = false;

        snprintf(

            calc.error,

            sizeof(calc.error),

            "Memory overflow"

        );

        return;

    }

    calc.has_memory = true;

    calc.error[0] = '\0';

}

static void memory_subtract(void)

{

    double value;

    if (!current_value(&value))

        return;

    calc.memory -= value;

    if (!isfinite(calc.memory)) {

        calc.memory = 0.0;

        calc.has_memory = false;

        snprintf(

            calc.error,

            sizeof(calc.error),

            "Memory overflow"

        );

        return;

    }

    calc.has_memory = true;

    calc.error[0] = '\0';

}

static void memory_clear(void)

{

    calc.memory = 0.0;

    calc.has_memory = false;

    calc.error[0] = '\0';

}

static void memory_recall(void)

{

    if (!calc.has_memory) {

        snprintf(calc.error, sizeof(calc.error), "Memory is empty");

        return;

    }

    append_text("MR");

}

/* ============================================================

   Key actions

   ============================================================ */

static void press_key(int index)
{
    const Key *keys = active_keypad();
    int count = active_key_count();
    if (index < 0 || index >= count) return;
    const char *insert = keys[index].insert;
    if (!strcmp(insert, "DEL")) { delete_last(); return; }
    if (!strcmp(insert, "AC")) { clear_expression(); return; }
    if (!strcmp(insert, "SIGN")) { toggle_sign(); return; }
    if (!strcmp(insert, "EQUAL")) { calculate(); return; }
    if (!strcmp(insert, "SECOND")) { calc.second_mode = !calc.second_mode; return; }
    if (!strcmp(insert, "RAD")) { calc.degrees = !calc.degrees; calc.error[0] = '\0'; return; }
    if (!strcmp(insert, "MC")) { memory_clear(); return; }
    if (!strcmp(insert, "M+")) { memory_add(); return; }
    if (!strcmp(insert, "M-")) { memory_subtract(); return; }
    if (!strcmp(insert, "MR")) { memory_recall(); return; }
    if (!strcmp(insert, "%")) { append_char('%'); return; }
    if (!strcmp(insert, "Ans")) { recall_ans(); return; }
    if (calc.second_mode) {
        if (!strcmp(insert, "^2")) insert = "sqrt(";
        else if (!strcmp(insert, "^3")) insert = "cbrt(";
        else if (!strcmp(insert, "sin(")) insert = "asin(";
        else if (!strcmp(insert, "cos(")) insert = "acos(";
        else if (!strcmp(insert, "tan(")) insert = "atan(";
        else if (!strcmp(insert, "sinh(")) insert = "asinh(";
        else if (!strcmp(insert, "cosh(")) insert = "acosh(";
        else if (!strcmp(insert, "tanh(")) insert = "atanh(";
        else if (!strcmp(insert, "sqrt(")) insert = "^2";
        else if (!strcmp(insert, "cbrt(")) insert = "^3";
        else if (!strcmp(insert, "^")) insert = "^(1/";
        else if (!strcmp(insert, "^(1/")) insert = "^";
        else if (!strcmp(insert, "exp(")) insert = "ln(";
        else if (!strcmp(insert, "tenpow(")) insert = "log(";
        calc.second_mode = false;
    } else {
        if (!strcmp(insert, "^2")) { append_text("^2"); return; }
        if (!strcmp(insert, "^3")) { append_text("^3"); return; }
    }
    append_text(insert);
}

/* ============================================================

   Drawing helpers

   ============================================================ */

static void draw_text(

    Screen screen,

    int x,

    int y,

    unsigned int color,

    const char *text)

{

    gfx_print(

        screen,

        x,

        y,

        FONT_MONO,

        color,

        "%s",

        text

    );

}

static void draw_centered_text(

    Screen screen,

    int x,

    int y,

    int width,

    unsigned int color,

    const char *text)

{

    int text_width = gfx_text_width(FONT_MONO, text);

    int text_x = x + (width - text_width) / 2;

    if (text_x < x)

        text_x = x;

    draw_text(screen, text_x, y, color, text);

}

/* ============================================================

   Top screen

   ============================================================ */

static void draw_top(void)

{

    gfx_clear(SCREEN_TOP, C_BG);

    gfx_rect(SCREEN_TOP, 0, 0, 400, 39, C_PANEL);

    draw_text(SCREEN_TOP, 14, 7, C_TEXT, "Calculator");

    draw_text(SCREEN_TOP, 14, 23, C_MUTED, calc.scientific_page ? "Scientific" : "Basic");

    gfx_round_rect(

        SCREEN_TOP,

        320,

        8,

        66,

        23,

        6,

        C_ACCENT_DARK

    );

    draw_centered_text(

        SCREEN_TOP,

        320,

        15,

        66,

        C_ACCENT,

        calc.degrees ? "DEG" : "RAD"

    );

    /* Expression card */

    gfx_round_rect(

        SCREEN_TOP,

        12,

        48,

        376,

        64,

        8,

        C_PANEL

    );

    draw_text(SCREEN_TOP, 23, 56, C_MUTED, "EXPRESSION");

    int expr_width = gfx_text_width(FONT_MONO, calc.expression);

    int expr_len = (int)strlen(calc.expression);

    if (expr_width <= 352) {

        draw_text(

            SCREEN_TOP,

            23,

            79,

            C_TEXT,

            calc.expression

        );

    } else {

        int start = expr_len > 44 ? expr_len - 44 : 0;

        draw_text(

            SCREEN_TOP,

            23,

            79,

            C_TEXT,

            calc.expression + start

        );

    }

    /* Result card */

    gfx_round_rect(

        SCREEN_TOP,

        12,

        120,

        376,

        77,

        8,

        C_PANEL

    );

    draw_text(SCREEN_TOP, 23, 129, C_MUTED, "RESULT");

    draw_text(

        SCREEN_TOP,

        23,

        151,

        C_TEXT,

        calc.result[0] ? calc.result : "0"

    );

    /* Footer */

    gfx_rect(SCREEN_TOP, 0, 204, 400, 36, C_PANEL);

    if (calc.error[0]) {

        draw_text(

            SCREEN_TOP,

            10,

            216,

            C_ERROR,

            calc.error

        );

    } else if (calc.has_memory) {

        char memory_text[40];

        format_number(

            calc.memory,

            memory_text,

            sizeof(memory_text)

        );

        draw_text(SCREEN_TOP, 10, 216, C_PURPLE, "M:");

        draw_text(SCREEN_TOP, 26, 216, C_MUTED, memory_text);

    } else {

        draw_text(SCREEN_TOP, 10, 216, C_MUTED, "A: select");

    }

    draw_text(SCREEN_TOP, 200, 216, C_MUTED, "X: history");

    draw_text(SCREEN_TOP, 300, 216, C_MUTED, calc.degrees ? "DEG" : "RAD");

}

/* ============================================================

   Bottom screen keypad

   ============================================================ */

static void draw_key(int index)
{
    const Key *keys = active_keypad();
    int cols = active_cols();
    int gap_x = calc.scientific_page ? 2 : 4;
    int gap_y = 3;
    int x0 = calc.scientific_page ? 3 : 5;
    int y0 = 31;
    int w = calc.scientific_page ? 50 : 74;
    int h = 37;
    int row = index / cols, col = index % cols;
    int x = x0 + col * (w + gap_x), y = y0 + row * (h + gap_y);
    unsigned int color = keys[index].color;
    if (index == calc.selected_key) color = C_ACCENT_DARK;
    gfx_round_rect(SCREEN_BOTTOM, x, y, w, h, h / 2, color);
    if (index == calc.selected_key) gfx_rect_outline(SCREEN_BOTTOM, x, y, w, h, 1, C_ACCENT);

    const char *label = keys[index].label;
    unsigned int text_color = C_TEXT;
    if (!calc.scientific_page && (index <= 2 || index == 18) && index != calc.selected_key)
        text_color = C_TEXT_DARK;
    if (calc.scientific_page) {
        if (index == 29) label = calc.degrees ? "DEG" : "RAD";
        if (calc.second_mode) {
            switch (index) {
                case 7:  label = "sqrt"; break;
                case 8:  label = "cbrt"; break;
                case 9:  label = "yroot"; break;
                case 10: label = "ln"; break;
                case 11: label = "log10"; break;
                case 15: label = "x^y"; break;
                case 19: label = "asin"; break;
                case 20: label = "acos"; break;
                case 21: label = "atan"; break;
                case 25: label = "asinh"; break;
                case 26: label = "acosh"; break;
                case 27: label = "atanh"; break;
                default: break;
            }
        }
    }
    draw_centered_text(SCREEN_BOTTOM, x, y + h / 2 - 4, w, text_color, label);
}

/* ============================================================

   History screen

   ============================================================ */

static void draw_history(void)

{

    gfx_clear(SCREEN_BOTTOM, C_BG);

    gfx_rect(SCREEN_BOTTOM, 0, 0, 320, 26, C_PANEL);

    draw_text(SCREEN_BOTTOM, 8, 8, C_ACCENT, "HISTORY");

    draw_text(SCREEN_BOTTOM, 248, 8, C_MUTED, "B: back");

    if (calc.history_count == 0) {

        draw_text(

            SCREEN_BOTTOM,

            12,

            48,

            C_MUTED,

            "No calculations"

        );

        return;

    }

    int y = 34;

    for (int i = calc.history_count - 1; i >= 0; i--) {

        gfx_round_rect(

            SCREEN_BOTTOM,

            7,

            y,

            306,

            23,

            4,

            C_PANEL

        );

        draw_text(

            SCREEN_BOTTOM,

            13,

            y + 3,

            C_TEXT,

            calc.history_expr[i]

        );

        draw_text(

            SCREEN_BOTTOM,

            13,

            y + 13,

            C_ACCENT,

            calc.history_result[i]

        );

        y += 26;

        if (y + 23 > 240)

            break;

    }

}

/* ============================================================

   Help screen

   ============================================================ */

static void draw_help(void)

{

    gfx_clear(SCREEN_BOTTOM, C_BG);

    gfx_rect(SCREEN_BOTTOM, 0, 0, 320, 26, C_PANEL);

    draw_text(SCREEN_BOTTOM, 8, 8, C_ACCENT, "HELP");

    draw_text(SCREEN_BOTTOM, 248, 8, C_MUTED, "B: back");

    draw_text(SCREEN_BOTTOM, 10, 40, C_TEXT, "A: select / tap a key");

    draw_text(SCREEN_BOTTOM, 10, 57, C_TEXT, "B: delete");

    draw_text(SCREEN_BOTTOM, 10, 74, C_TEXT, "X: history");

    draw_text(SCREEN_BOTTOM, 10, 91, C_TEXT, "Y: DEG / RAD");

    draw_text(SCREEN_BOTTOM, 10, 108, C_TEXT, "START: calculate");

    draw_text(SCREEN_BOTTOM, 10, 125, C_TEXT, "L: 2nd mode");

    draw_text(SCREEN_BOTTOM, 10, 142, C_TEXT, "R: switch page");

    draw_text(SCREEN_BOTTOM, 10, 159, C_TEXT, "History: Up/Down recalls an entry");

    draw_text(SCREEN_BOTTOM, 10, 176, C_TEXT, "SELECT: clear memory");

    draw_text(SCREEN_BOTTOM, 10, 202, C_MUTED, "Arrows: navigation; tap title for help");

}

/* ============================================================

   Bottom screen

   ============================================================ */

static void draw_bottom(void)
{
    if (calc.show_history) { draw_history(); return; }
    if (calc.show_help) { draw_help(); return; }
    gfx_clear(SCREEN_BOTTOM, C_BG);
    gfx_rect(SCREEN_BOTTOM, 0, 0, 320, 26, C_PANEL);
    draw_text(SCREEN_BOTTOM, 8, 8, C_TEXT, calc.scientific_page ? "SCIENTIFIC" : "BASIC");
    draw_text(SCREEN_BOTTOM, 105, 8, C_MUTED, "R: page");
    if (calc.scientific_page) {
        draw_text(SCREEN_BOTTOM, 175, 8, C_MUTED, calc.second_mode ? "2nd ON" : "2nd OFF");
        draw_text(SCREEN_BOTTOM, 260, 8, C_MUTED, calc.degrees ? "DEG" : "RAD");
        for (int i = 0; i < SCI_KEY_COUNT; i++) draw_key(i);
    } else {
        draw_text(SCREEN_BOTTOM, 190, 8, C_MUTED, "X:hist");
        draw_text(SCREEN_BOTTOM, 260, 8, C_MUTED, calc.degrees ? "DEG" : "RAD");
        for (int i = 0; i < BASIC_KEY_COUNT; i++) draw_key(i);
    }
}

static void render(void)

{

    draw_top();

    draw_bottom();

}

/* ============================================================

   Physical input

   ============================================================ */

static void move_selection(int dx, int dy)
{
    int cols=active_cols(), rows=active_rows(), count=active_key_count();
    if (calc.selected_key >= count) calc.selected_key = 0;
    int row=calc.selected_key/cols, col=calc.selected_key%cols;
    col=(col+dx+cols)%cols; row=(row+dy+rows)%rows;
    calc.selected_key=row*cols+col;
    if (calc.selected_key>=count) calc.selected_key=count-1;
}

static void handle_physical_input(void)

{

    uint32_t down = hid_keys_down();

    if (calc.show_history || calc.show_help) {
        if (calc.show_history && (down & KEY_UP)) {
            recall_history(1);
            return;
        }
        if (calc.show_history && (down & KEY_DOWN)) {
            recall_history(-1);
            return;
        }
        if (down & (KEY_A | KEY_B | KEY_X)) {
            calc.show_history = false;
            calc.show_help = false;
        }
        return;
    }

    if (down & KEY_UP)

        move_selection(0, -1);

    if (down & KEY_DOWN)

        move_selection(0, 1);

    if (down & KEY_LEFT)

        move_selection(-1, 0);

    if (down & KEY_RIGHT)

        move_selection(1, 0);

    if (down & KEY_A) {
        if (calc.selected_key >= active_key_count()) calc.selected_key = 0;
        press_key(calc.selected_key);
    }

    if (down & KEY_B) delete_last();

    if (down & KEY_X) {
        calc.show_history = !calc.show_history;
        calc.show_help = false;
    }

    if (down & KEY_Y) {

        calc.degrees = !calc.degrees;

        calc.error[0] = '\0';

    }

    if (down & KEY_START) calculate();

    if (down & KEY_L) {
        if (calc.scientific_page) calc.second_mode = !calc.second_mode;
    }
    if (down & KEY_R) {
        calc.scientific_page = !calc.scientific_page;
        calc.selected_key = 0;
        calc.second_mode = false;
    }
    if (down & KEY_SELECT) memory_clear();

}

/* ============================================================

   Touch input

   ============================================================ */

static void handle_touch_input(void)
{
    if (!hid_touch_down()) return;
    int x = hid_touch_x(), y = hid_touch_y();
    if (x < 0 || x >= 320 || y < 0 || y >= 240) return;
    if (calc.show_history) {
        if (y >= 34 && calc.history_count > 0) {
            int row = (y - 34) / 26;
            int item_y = 34 + row * 26;
            if (row < calc.history_count && y >= item_y && y < item_y + 23) {
                int item = calc.history_count - 1 - row;
                snprintf(calc.expression, sizeof(calc.expression), "%s", calc.history_expr[item]);
                calc.result[0] = '\0';
                calc.error[0] = '\0';
                calc.history_position = item;
            }
        }
        calc.show_history = false;
        return;
    }
    if (calc.show_help) {
        calc.show_help = false;
        return;
    }
    if (y < 26) {
        if (x < 60) {
            calc.show_help = true;
        } else if (x >= 100 && x < 170) {
            calc.scientific_page = !calc.scientific_page;
            calc.selected_key = 0;
            calc.second_mode = false;
        } else if (x >= 250) {
            calc.degrees = !calc.degrees;
        } else if (!calc.scientific_page && x >= 185) {
            calc.show_history = true;
        } else if (calc.scientific_page && x >= 170 && x < 245) {
            calc.second_mode = !calc.second_mode;
        }
        calc.error[0] = '\0';
        return;
    }
    int cols = active_cols(), count = active_key_count();
    int gap_x = calc.scientific_page ? 2 : 4, gap_y = 3;
    int x0 = calc.scientific_page ? 3 : 5, y0 = 31;
    int w = calc.scientific_page ? 50 : 74, h = 37;
    int col = (x - x0) / (w + gap_x), row = (y - y0) / (h + gap_y);
    if (col < 0 || col >= cols || row < 0 || row >= 5) return;
    int bx = x0 + col * (w + gap_x), by = y0 + row * (h + gap_y);
    if (x < bx || x >= bx + w || y < by || y >= by + h) return;
    int index = row * cols + col;
    if (index >= count) return;
    calc.selected_key = index;
    press_key(index);
}

/* ============================================================

   Initialization

   ============================================================ */

static void initialize(void)

{

    memset(&calc, 0, sizeof(calc));

    calc.degrees = true;

    calc.selected_key = 19;
    calc.scientific_page = false;

    calc.history_position = 0;
    calc.second_mode = false;
    srand((unsigned int)time(NULL));

}

/* ============================================================

   Main

   ============================================================ */

int main(void)

{

    initialize();

    while (app_loop()) {

        handle_physical_input();

        handle_touch_input();

        render();

    }

    return 0;

}