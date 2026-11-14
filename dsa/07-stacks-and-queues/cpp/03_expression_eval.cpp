/*
 * 03_expression_eval.cpp — brackets, shunting-yard and RPN with std::stack
 *
 * Concept : Same algorithms as c/06_brackets_and_postfix.c, but with
 *           std::stack, std::string_view for zero-copy input, a token
 *           vector instead of a char buffer, and std::optional for errors.
 *           Shows how much bookkeeping (buffer sizes, overflow) disappears.
 * Build   : make run P=dsa/07-stacks-and-queues/cpp/03_expression_eval.cpp
 * Read    : ../README.md  (section "Applications")
 */
#include <cassert>
#include <cctype>
#include <iostream>
#include <optional>
#include <stack>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// O(n). A map from closer to opener keeps the matching rule in one place.
bool balanced(std::string_view s)
{
    static const std::unordered_map<char, char> match{{')', '('}, {']', '['}, {'}', '{'}};
    std::stack<char> open;
    for (char c : s) {
        if (c == '(' || c == '[' || c == '{') {
            open.push(c);
        } else if (auto it = match.find(c); it != match.end()) {
            if (open.empty() || open.top() != it->second) return false;
            open.pop();
        }
    }
    return open.empty();
}

using Tokens = std::vector<std::string>;

int prec(char op)
{
    switch (op) {
    case '+': case '-': return 1;
    case '*': case '/': return 2;
    case '^': return 3;
    default: return 0;
    }
}

// Shunting-yard, O(n). Returns nullopt on mismatched parentheses.
std::optional<Tokens> to_rpn(std::string_view in)
{
    Tokens out;
    std::stack<char> ops;
    for (std::size_t i = 0; i < in.size(); ++i) {
        char c = in[i];
        if (std::isspace(static_cast<unsigned char>(c))) continue;
        if (std::isdigit(static_cast<unsigned char>(c))) {
            std::size_t j = i;
            while (j < in.size() && std::isdigit(static_cast<unsigned char>(in[j]))) ++j;
            out.emplace_back(in.substr(i, j - i));  // string_view -> string
            i = j - 1;
        } else if (c == '(') {
            ops.push(c);
        } else if (c == ')') {
            while (!ops.empty() && ops.top() != '(') {
                out.emplace_back(1, ops.top());
                ops.pop();
            }
            if (ops.empty()) return std::nullopt;
            ops.pop();
        } else {
            // pop while top binds tighter, or equally tight and c is left-assoc
            while (!ops.empty() && ops.top() != '(' &&
                   (prec(ops.top()) > prec(c) || (prec(ops.top()) == prec(c) && c != '^'))) {
                out.emplace_back(1, ops.top());
                ops.pop();
            }
            ops.push(c);
        }
    }
    while (!ops.empty()) {
        if (ops.top() == '(') return std::nullopt;
        out.emplace_back(1, ops.top());
        ops.pop();
    }
    return out;
}

// RPN evaluation, O(n). First pop = RIGHT operand.
std::optional<long> eval_rpn(const Tokens& toks)
{
    std::stack<long, std::vector<long>> st;
    for (const auto& t : toks) {
        if (std::isdigit(static_cast<unsigned char>(t[0]))) {
            st.push(std::stol(t));
            continue;
        }
        if (st.size() < 2) return std::nullopt;
        long b = st.top(); st.pop();
        long a = st.top(); st.pop();
        switch (t[0]) {
        case '+': st.push(a + b); break;
        case '-': st.push(a - b); break;
        case '*': st.push(a * b); break;
        case '/':
            if (b == 0) return std::nullopt;
            st.push(a / b);
            break;
        case '^': {
            long r = 1;
            for (long k = 0; k < b; ++k) r *= a;
            st.push(r);
            break;
        }
        default: return std::nullopt;
        }
    }
    if (st.size() != 1) return std::nullopt;
    return st.top();
}

std::string join(const Tokens& t)
{
    std::string s;
    for (const auto& x : t) s += (s.empty() ? "" : " ") + x;
    return s;
}

int main()
{
    for (std::string_view s : {"{[()]}", "", "a(b)c[d]"}) assert(balanced(s));
    for (std::string_view s : {"(]", "((", "())"}) assert(!balanced(s));
    std::cout << "brackets: ok\n";

    struct Case {
        std::string_view infix;
        std::string rpn;
        long value;
    };
    const Case cases[] = {
        {"3 + 4 * 2 - 1", "3 4 2 * + 1 -", 10},
        {"(3 + 4) * 2", "3 4 + 2 *", 14},
        {"100 / 10 / 2", "100 10 / 2 /", 5},
        {"2 ^ 3 ^ 2", "2 3 2 ^ ^", 512},
        {"12*(5-2)+7", "12 5 2 - * 7 +", 43},
    };
    for (const auto& c : cases) {
        auto rpn = to_rpn(c.infix);
        assert(rpn);
        auto v = eval_rpn(*rpn);
        assert(v);
        std::cout << "  " << c.infix << "  ->  " << join(*rpn) << "  =  " << *v << '\n';
        assert(join(*rpn) == c.rpn);
        assert(*v == c.value);
    }
    assert(!to_rpn("(1+2"));
    assert(!to_rpn("1+2)"));
    assert(!eval_rpn({"1", "+"}));
    assert(!eval_rpn({"4", "0", "/"}));

    std::cout << "03_expression_eval: all checks passed\n";
    return 0;
}
