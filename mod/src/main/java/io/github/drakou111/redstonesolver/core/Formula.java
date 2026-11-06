package io.github.drakou111.redstonesolver.core;

import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.TreeSet;

/**
 * An output written as a formula of the inputs, such as {@code (A + B) % 16} or
 * {@code A + B >= 16}. Integers only. A formula whose outermost operation is a comparison or a
 * logical operation is on/off; any other formula gives the exact signal strength.
 *
 * <p>Operators, loosest first: {@code ? :}, {@code or ||}, {@code and &&}, {@code == !=},
 * {@code < <= > >=}, {@code + -}, {@code * / %}, then {@code not ! -} and calls to
 * {@code min(a, b)}, {@code max(a, b)} and {@code abs(a)}. Zero is false, anything else true.
 */
public final class Formula {
    private final String text;
    private final Node root;

    private Formula(String text, Node root) {
        this.text = text;
        this.root = root;
    }

    /** Parses {@code text}; throws IllegalArgumentException saying what is wrong and where. */
    public static Formula parse(String text) {
        Parser parser = new Parser(tokenize(text), text);
        Node root = parser.ternary();
        parser.expectEnd();
        return new Formula(text.trim(), root);
    }

    public String getText() {
        return this.text;
    }

    /** True when the result means on (non-zero) or off (zero) rather than a signal strength. */
    public boolean isOnOff() {
        return this.root.isBoolean();
    }

    /** Every name the formula reads, sorted. */
    public Set<String> variables() {
        Set<String> names = new TreeSet<>();
        this.root.collect(names);
        return names;
    }

    public int evaluate(Map<String, Integer> values) {
        return this.root.eval(values);
    }

    /** The output range this formula asks for at these input values. */
    public SignalRange outputFor(Map<String, Integer> values) {
        int result = this.evaluate(values);
        if (this.isOnOff()) return result != 0 ? new SignalRange(1, SignalRange.MAX_SIGNAL) : new SignalRange(0, 0);
        if (result < 0 || result > SignalRange.MAX_SIGNAL)
            throw new IllegalArgumentException("gives %d for %s; outputs must be 0..15".formatted(result, values));
        return new SignalRange(result, result);
    }

    /** {@code text} with every use of one name replaced, keeping everything else as typed. */
    public static String renameVariable(String text, String oldName, String newName) {
        StringBuilder result = new StringBuilder();
        int i = 0;
        while (i < text.length()) {
            char c = text.charAt(i);
            if (Character.isLetter(c) || c == '_') {
                int start = i;
                while (i < text.length() && (Character.isLetterOrDigit(text.charAt(i)) || text.charAt(i) == '_')) i++;
                String word = text.substring(start, i);
                result.append(word.equals(oldName) ? newName : word);
                continue;
            }
            result.append(c);
            i++;
        }
        return result.toString();
    }

    // ------------------------------------------------------------------ syntax tree

    private sealed interface Node permits Num, Var, Unary, Binary, Ternary, Call {
        int eval(Map<String, Integer> values);

        boolean isBoolean();

        void collect(Set<String> names);
    }

    private record Num(int value) implements Node {
        public int eval(Map<String, Integer> values) {
            return this.value;
        }

        public boolean isBoolean() {
            return false;
        }

        public void collect(Set<String> names) {
        }
    }

    private record Var(String name) implements Node {
        public int eval(Map<String, Integer> values) {
            Integer value = values.get(this.name);
            if (value == null) throw new IllegalArgumentException("there is no input named " + this.name);
            return value;
        }

        public boolean isBoolean() {
            return false;
        }

        public void collect(Set<String> names) {
            names.add(this.name);
        }
    }

    private record Unary(String op, Node operand) implements Node {
        public int eval(Map<String, Integer> values) {
            int value = this.operand.eval(values);
            return switch (this.op) {
                case "-" -> -value;
                case "not" -> value == 0 ? 1 : 0;
                default -> throw new IllegalStateException(this.op);
            };
        }

        public boolean isBoolean() {
            return this.op.equals("not");
        }

        public void collect(Set<String> names) {
            this.operand.collect(names);
        }
    }

    private record Binary(String op, Node left, Node right) implements Node {
        public int eval(Map<String, Integer> values) {
            if (this.op.equals("and")) return this.left.eval(values) != 0 && this.right.eval(values) != 0 ? 1 : 0;
            if (this.op.equals("or")) return this.left.eval(values) != 0 || this.right.eval(values) != 0 ? 1 : 0;
            int a = this.left.eval(values);
            int b = this.right.eval(values);
            return switch (this.op) {
                case "+" -> a + b;
                case "-" -> a - b;
                case "*" -> a * b;
                case "/" -> {
                    if (b == 0) throw new IllegalArgumentException("divides by zero for " + values);
                    yield Math.floorDiv(a, b);
                }
                case "%" -> {
                    if (b == 0) throw new IllegalArgumentException("takes % 0 for " + values);
                    yield Math.floorMod(a, b);
                }
                case "<" -> a < b ? 1 : 0;
                case "<=" -> a <= b ? 1 : 0;
                case ">" -> a > b ? 1 : 0;
                case ">=" -> a >= b ? 1 : 0;
                case "==" -> a == b ? 1 : 0;
                case "!=" -> a != b ? 1 : 0;
                default -> throw new IllegalStateException(this.op);
            };
        }

        public boolean isBoolean() {
            return switch (this.op) {
                case "<", "<=", ">", ">=", "==", "!=", "and", "or" -> true;
                default -> false;
            };
        }

        public void collect(Set<String> names) {
            this.left.collect(names);
            this.right.collect(names);
        }
    }

    private record Ternary(Node condition, Node then, Node otherwise) implements Node {
        public int eval(Map<String, Integer> values) {
            return this.condition.eval(values) != 0 ? this.then.eval(values) : this.otherwise.eval(values);
        }

        public boolean isBoolean() {
            return this.then.isBoolean() && this.otherwise.isBoolean();
        }

        public void collect(Set<String> names) {
            this.condition.collect(names);
            this.then.collect(names);
            this.otherwise.collect(names);
        }
    }

    private record Call(String function, List<Node> arguments) implements Node {
        public int eval(Map<String, Integer> values) {
            int first = this.arguments.getFirst().eval(values);
            return switch (this.function) {
                case "abs" -> Math.abs(first);
                case "min" -> Math.min(first, this.arguments.get(1).eval(values));
                case "max" -> Math.max(first, this.arguments.get(1).eval(values));
                default -> throw new IllegalStateException(this.function);
            };
        }

        public boolean isBoolean() {
            return false;
        }

        public void collect(Set<String> names) {
            for (Node argument : this.arguments) argument.collect(names);
        }
    }

    // ------------------------------------------------------------------ parsing

    private record Token(String text, int position, boolean number, boolean word) {
    }

    private static final List<String> SYMBOLS = List.of("<=", ">=", "==", "!=", "&&", "||", "+", "-", "*", "/", "%",
            "<", ">", "!", "(", ")", ",", "?", ":");

    private static List<Token> tokenize(String text) {
        List<Token> tokens = new ArrayList<>();
        int i = 0;
        while (i < text.length()) {
            char c = text.charAt(i);
            if (Character.isWhitespace(c)) {
                i++;
                continue;
            }
            int start = i;
            if (Character.isDigit(c)) {
                while (i < text.length() && Character.isDigit(text.charAt(i))) i++;
                tokens.add(new Token(text.substring(start, i), start, true, false));
                continue;
            }
            if (Character.isLetter(c) || c == '_') {
                while (i < text.length() && (Character.isLetterOrDigit(text.charAt(i)) || text.charAt(i) == '_')) i++;
                tokens.add(new Token(text.substring(start, i), start, false, true));
                continue;
            }
            String symbol = null;
            for (String candidate : SYMBOLS) {
                if (text.startsWith(candidate, i)) {
                    symbol = candidate;
                    break;
                }
            }
            if (symbol == null) throw new IllegalArgumentException("unexpected '" + c + "' at character " + (i + 1));
            // Word and symbol spellings mean the same.
            String normalized = switch (symbol) {
                case "&&" -> "and";
                case "||" -> "or";
                case "!" -> "not";
                default -> symbol;
            };
            tokens.add(new Token(normalized, start, false, false));
            i += symbol.length();
        }
        return tokens;
    }

    private static final class Parser {
        private final List<Token> tokens;
        private final String text;
        private int next;

        Parser(List<Token> tokens, String text) {
            this.tokens = tokens;
            this.text = text;
        }

        Node ternary() {
            Node condition = this.or();
            if (!this.accept("?")) return condition;
            Node then = this.ternary();
            this.expect(":");
            Node otherwise = this.ternary();
            return new Ternary(condition, then, otherwise);
        }

        private Node or() {
            Node left = this.and();
            while (this.acceptWord("or")) left = new Binary("or", left, this.and());
            return left;
        }

        private Node and() {
            Node left = this.equality();
            while (this.acceptWord("and")) left = new Binary("and", left, this.equality());
            return left;
        }

        private Node equality() {
            Node left = this.comparison();
            while (this.peekIs("==") || this.peekIs("!=")) {
                String op = this.tokens.get(this.next++).text();
                left = new Binary(op, left, this.comparison());
            }
            return left;
        }

        private Node comparison() {
            Node left = this.sum();
            while (this.peekIs("<") || this.peekIs("<=") || this.peekIs(">") || this.peekIs(">=")) {
                String op = this.tokens.get(this.next++).text();
                left = new Binary(op, left, this.sum());
            }
            return left;
        }

        private Node sum() {
            Node left = this.product();
            while (this.peekIs("+") || this.peekIs("-")) {
                String op = this.tokens.get(this.next++).text();
                left = new Binary(op, left, this.product());
            }
            return left;
        }

        private Node product() {
            Node left = this.unary();
            while (this.peekIs("*") || this.peekIs("/") || this.peekIs("%")) {
                String op = this.tokens.get(this.next++).text();
                left = new Binary(op, left, this.unary());
            }
            return left;
        }

        private Node unary() {
            if (this.accept("-")) return new Unary("-", this.unary());
            if (this.acceptWord("not")) return new Unary("not", this.unary());
            return this.primary();
        }

        private Node primary() {
            if (this.next >= this.tokens.size()) throw this.error("the formula ends too early");
            Token token = this.tokens.get(this.next++);
            if (token.number()) {
                try {
                    return new Num(Integer.parseInt(token.text()));
                } catch (NumberFormatException e) {
                    throw new IllegalArgumentException("the number " + token.text() + " is too big");
                }
            }
            if (token.text().equals("(")) {
                Node inside = this.ternary();
                this.expect(")");
                return inside;
            }
            if (!token.word()) throw this.errorAt(token, "expected a number, an input name or (");
            String name = token.text();
            if (name.equals("min") || name.equals("max") || name.equals("abs")) {
                this.expect("(");
                List<Node> arguments = new ArrayList<>();
                arguments.add(this.ternary());
                while (this.accept(",")) arguments.add(this.ternary());
                this.expect(")");
                int wanted = name.equals("abs") ? 1 : 2;
                if (arguments.size() != wanted)
                    throw this.errorAt(token, name + " takes " + wanted + (wanted == 1 ? " value" : " values"));
                return new Call(name, List.copyOf(arguments));
            }
            if (name.equals("and") || name.equals("or") || name.equals("not"))
                throw this.errorAt(token, "'" + name + "' needs a value before it");
            return new Var(name);
        }

        void expectEnd() {
            if (this.next < this.tokens.size())
                throw this.errorAt(this.tokens.get(this.next), "unexpected '" + this.tokens.get(this.next).text() + "'");
        }

        private boolean peekIs(String text) {
            return this.next < this.tokens.size() && !this.tokens.get(this.next).word()
                    && this.tokens.get(this.next).text().equals(text);
        }

        private boolean accept(String text) {
            if (!this.peekIs(text)) return false;
            this.next++;
            return true;
        }

        private boolean acceptWord(String word) {
            if (this.next >= this.tokens.size() || !this.tokens.get(this.next).text().equals(word)) return false;
            this.next++;
            return true;
        }

        private void expect(String text) {
            if (!this.accept(text)) {
                if (this.next >= this.tokens.size()) throw this.error("missing '" + text + "' at the end");
                throw this.errorAt(this.tokens.get(this.next), "expected '" + text + "'");
            }
        }

        private IllegalArgumentException errorAt(Token token, String message) {
            return new IllegalArgumentException(message + " at character " + (token.position() + 1));
        }

        private IllegalArgumentException error(String message) {
            return new IllegalArgumentException(this.text.isBlank() ? "the formula is empty" : message);
        }
    }
}
