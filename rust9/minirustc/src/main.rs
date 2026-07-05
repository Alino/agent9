//! minirustc — a small native Rust compiler that RUNS ON 9front.
//!
//! It compiles a real subset of Rust (functions, recursion, i64 arithmetic,
//! let/mut, if/else, while, calls, println!) to C, then drives the on-box kencc
//! toolchain (`6c`/`6l`) via std::process to produce a native 9front a.out — and
//! runs it. Cross-compiled for 9front with rust9; the whole compile pipeline
//! executes on the box. Not rustc, but a genuine Rust compiler on Plan 9.
use std::process::Command;

// ---------------------------------------------------------------- lexer
#[derive(Clone, Debug, PartialEq)]
enum Tok {
    Ident(String),
    Int(i64),
    Str(String),
    // keywords
    Fn,
    Let,
    Mut,
    If,
    Else,
    While,
    Return,
    // punctuation / operators
    LParen,
    RParen,
    LBrace,
    RBrace,
    Comma,
    Semi,
    Colon,
    Arrow, // ->
    Bang,  // !
    Assign,
    Plus,
    Minus,
    Star,
    Slash,
    Percent,
    EqEq,
    Ne,
    Lt,
    Le,
    Gt,
    Ge,
    AndAnd,
    OrOr,
    Eof,
}

fn lex(src: &str) -> Result<Vec<Tok>, String> {
    let b = src.as_bytes();
    let mut i = 0;
    let mut out = Vec::new();
    while i < b.len() {
        let c = b[i];
        if c == b' ' || c == b'\t' || c == b'\r' || c == b'\n' {
            i += 1;
            continue;
        }
        // line comment
        if c == b'/' && i + 1 < b.len() && b[i + 1] == b'/' {
            while i < b.len() && b[i] != b'\n' {
                i += 1;
            }
            continue;
        }
        if c.is_ascii_digit() {
            let s = i;
            while i < b.len() && b[i].is_ascii_digit() {
                i += 1;
            }
            let n: i64 = src[s..i].parse().map_err(|_| "bad int".to_string())?;
            out.push(Tok::Int(n));
            continue;
        }
        if c == b'"' {
            i += 1;
            let s = i;
            while i < b.len() && b[i] != b'"' {
                i += 1;
            }
            let text = src[s..i].to_string();
            i += 1; // closing quote
            out.push(Tok::Str(text));
            continue;
        }
        if c.is_ascii_alphabetic() || c == b'_' {
            let s = i;
            while i < b.len() && (b[i].is_ascii_alphanumeric() || b[i] == b'_') {
                i += 1;
            }
            let word = &src[s..i];
            out.push(match word {
                "fn" => Tok::Fn,
                "let" => Tok::Let,
                "mut" => Tok::Mut,
                "if" => Tok::If,
                "else" => Tok::Else,
                "while" => Tok::While,
                "return" => Tok::Return,
                _ => Tok::Ident(word.to_string()),
            });
            continue;
        }
        // two-char operators
        let two = if i + 1 < b.len() { &src[i..i + 2] } else { "" };
        let t = match two {
            "->" => Some(Tok::Arrow),
            "==" => Some(Tok::EqEq),
            "!=" => Some(Tok::Ne),
            "<=" => Some(Tok::Le),
            ">=" => Some(Tok::Ge),
            "&&" => Some(Tok::AndAnd),
            "||" => Some(Tok::OrOr),
            _ => None,
        };
        if let Some(t) = t {
            out.push(t);
            i += 2;
            continue;
        }
        let t = match c {
            b'(' => Tok::LParen,
            b')' => Tok::RParen,
            b'{' => Tok::LBrace,
            b'}' => Tok::RBrace,
            b',' => Tok::Comma,
            b';' => Tok::Semi,
            b':' => Tok::Colon,
            b'!' => Tok::Bang,
            b'=' => Tok::Assign,
            b'+' => Tok::Plus,
            b'-' => Tok::Minus,
            b'*' => Tok::Star,
            b'/' => Tok::Slash,
            b'%' => Tok::Percent,
            b'<' => Tok::Lt,
            b'>' => Tok::Gt,
            _ => return Err(format!("unexpected char '{}'", c as char)),
        };
        out.push(t);
        i += 1;
    }
    out.push(Tok::Eof);
    Ok(out)
}

// ---------------------------------------------------------------- AST
#[derive(Debug)]
enum Expr {
    Int(i64),
    Var(String),
    Unary(char, Box<Expr>),
    Bin(Tok, Box<Expr>, Box<Expr>),
    Call(String, Vec<Expr>),
}

#[derive(Debug)]
enum Stmt {
    Let(String, Expr),
    Assign(String, Expr),
    Expr(Expr),
    Return(Option<Expr>),
    If(Expr, Vec<Stmt>, Vec<Stmt>),
    While(Expr, Vec<Stmt>),
    Print(Expr), // print(e) / println!("{}", e)
}

struct Func {
    name: String,
    params: Vec<String>,
    body: Vec<Stmt>,
    is_main: bool,
}

// ---------------------------------------------------------------- parser
struct Parser {
    t: Vec<Tok>,
    p: usize,
}

impl Parser {
    fn peek(&self) -> &Tok {
        &self.t[self.p]
    }
    fn next(&mut self) -> Tok {
        let t = self.t[self.p].clone();
        self.p += 1;
        t
    }
    fn eat(&mut self, t: Tok) -> Result<(), String> {
        if *self.peek() == t {
            self.p += 1;
            Ok(())
        } else {
            Err(format!("expected {:?}, got {:?}", t, self.peek()))
        }
    }
    fn ident(&mut self) -> Result<String, String> {
        match self.next() {
            Tok::Ident(s) => Ok(s),
            other => Err(format!("expected ident, got {other:?}")),
        }
    }

    fn program(&mut self) -> Result<Vec<Func>, String> {
        let mut fns = Vec::new();
        while *self.peek() != Tok::Eof {
            fns.push(self.func()?);
        }
        Ok(fns)
    }

    fn func(&mut self) -> Result<Func, String> {
        self.eat(Tok::Fn)?;
        let name = self.ident()?;
        self.eat(Tok::LParen)?;
        let mut params = Vec::new();
        while *self.peek() != Tok::RParen {
            let pn = self.ident()?;
            if *self.peek() == Tok::Colon {
                self.next();
                self.ident()?; // type, ignored (assume i64)
            }
            params.push(pn);
            if *self.peek() == Tok::Comma {
                self.next();
            }
        }
        self.eat(Tok::RParen)?;
        if *self.peek() == Tok::Arrow {
            self.next();
            self.ident()?; // return type, ignored
        }
        let body = self.block()?;
        let is_main = name == "main";
        Ok(Func { name, params, body, is_main })
    }

    fn block(&mut self) -> Result<Vec<Stmt>, String> {
        self.eat(Tok::LBrace)?;
        let mut stmts = Vec::new();
        while *self.peek() != Tok::RBrace {
            stmts.push(self.stmt()?);
        }
        self.eat(Tok::RBrace)?;
        Ok(stmts)
    }

    fn stmt(&mut self) -> Result<Stmt, String> {
        match self.peek().clone() {
            Tok::Let => {
                self.next();
                if *self.peek() == Tok::Mut {
                    self.next();
                }
                let name = self.ident()?;
                if *self.peek() == Tok::Colon {
                    self.next();
                    self.ident()?; // type
                }
                self.eat(Tok::Assign)?;
                let e = self.expr()?;
                self.eat(Tok::Semi)?;
                Ok(Stmt::Let(name, e))
            }
            Tok::Return => {
                self.next();
                if *self.peek() == Tok::Semi {
                    self.next();
                    Ok(Stmt::Return(None))
                } else {
                    let e = self.expr()?;
                    self.eat(Tok::Semi)?;
                    Ok(Stmt::Return(Some(e)))
                }
            }
            Tok::If => {
                self.next();
                let cond = self.expr()?;
                let then = self.block()?;
                let els = if *self.peek() == Tok::Else {
                    self.next();
                    self.block()?
                } else {
                    Vec::new()
                };
                Ok(Stmt::If(cond, then, els))
            }
            Tok::While => {
                self.next();
                let cond = self.expr()?;
                let body = self.block()?;
                Ok(Stmt::While(cond, body))
            }
            Tok::Ident(name) => {
                // println!("{}", e)   or   assignment   or   expr (e.g. a call)
                if name == "println" && self.t.get(self.p + 1) == Some(&Tok::Bang) {
                    self.next(); // println
                    self.next(); // !
                    self.eat(Tok::LParen)?;
                    // "{}" , expr
                    if let Tok::Str(_) = self.peek() {
                        self.next();
                    }
                    self.eat(Tok::Comma)?;
                    let e = self.expr()?;
                    self.eat(Tok::RParen)?;
                    self.eat(Tok::Semi)?;
                    return Ok(Stmt::Print(e));
                }
                // lookahead: ident '=' -> assignment
                if self.t.get(self.p + 1) == Some(&Tok::Assign) {
                    self.next(); // ident
                    self.next(); // =
                    let e = self.expr()?;
                    self.eat(Tok::Semi)?;
                    return Ok(Stmt::Assign(name, e));
                }
                let e = self.expr()?;
                self.eat(Tok::Semi)?;
                Ok(Stmt::Expr(e))
            }
            _ => {
                let e = self.expr()?;
                self.eat(Tok::Semi)?;
                Ok(Stmt::Expr(e))
            }
        }
    }

    // precedence-climbing expression parser
    fn expr(&mut self) -> Result<Expr, String> {
        self.bin(0)
    }
    fn bin(&mut self, min_prec: u8) -> Result<Expr, String> {
        let mut lhs = self.unary()?;
        loop {
            let (prec, tok) = match self.peek() {
                Tok::OrOr => (1, Tok::OrOr),
                Tok::AndAnd => (2, Tok::AndAnd),
                Tok::EqEq => (3, Tok::EqEq),
                Tok::Ne => (3, Tok::Ne),
                Tok::Lt => (4, Tok::Lt),
                Tok::Le => (4, Tok::Le),
                Tok::Gt => (4, Tok::Gt),
                Tok::Ge => (4, Tok::Ge),
                Tok::Plus => (5, Tok::Plus),
                Tok::Minus => (5, Tok::Minus),
                Tok::Star => (6, Tok::Star),
                Tok::Slash => (6, Tok::Slash),
                Tok::Percent => (6, Tok::Percent),
                _ => break,
            };
            if prec < min_prec {
                break;
            }
            self.next();
            let rhs = self.bin(prec + 1)?;
            lhs = Expr::Bin(tok, Box::new(lhs), Box::new(rhs));
        }
        Ok(lhs)
    }
    fn unary(&mut self) -> Result<Expr, String> {
        match self.peek() {
            Tok::Minus => {
                self.next();
                Ok(Expr::Unary('-', Box::new(self.unary()?)))
            }
            Tok::Bang => {
                self.next();
                Ok(Expr::Unary('!', Box::new(self.unary()?)))
            }
            _ => self.primary(),
        }
    }
    fn primary(&mut self) -> Result<Expr, String> {
        match self.next() {
            Tok::Int(n) => Ok(Expr::Int(n)),
            Tok::LParen => {
                let e = self.expr()?;
                self.eat(Tok::RParen)?;
                Ok(e)
            }
            Tok::Ident(name) => {
                if *self.peek() == Tok::LParen {
                    self.next();
                    let mut args = Vec::new();
                    while *self.peek() != Tok::RParen {
                        args.push(self.expr()?);
                        if *self.peek() == Tok::Comma {
                            self.next();
                        }
                    }
                    self.eat(Tok::RParen)?;
                    Ok(Expr::Call(name, args))
                } else {
                    Ok(Expr::Var(name))
                }
            }
            other => Err(format!("unexpected token in expr: {other:?}")),
        }
    }
}

// ---------------------------------------------------------------- codegen (-> Plan 9 C)
fn cg_expr(e: &Expr, out: &mut String) {
    match e {
        Expr::Int(n) => out.push_str(&n.to_string()),
        Expr::Var(v) => out.push_str(v),
        Expr::Unary(op, e) => {
            out.push(*op);
            out.push('(');
            cg_expr(e, out);
            out.push(')');
        }
        Expr::Bin(op, a, b) => {
            out.push('(');
            cg_expr(a, out);
            out.push_str(match op {
                Tok::Plus => "+",
                Tok::Minus => "-",
                Tok::Star => "*",
                Tok::Slash => "/",
                Tok::Percent => "%",
                Tok::EqEq => "==",
                Tok::Ne => "!=",
                Tok::Lt => "<",
                Tok::Le => "<=",
                Tok::Gt => ">",
                Tok::Ge => ">=",
                Tok::AndAnd => "&&",
                Tok::OrOr => "||",
                _ => "?",
            });
            cg_expr(b, out);
            out.push(')');
        }
        Expr::Call(name, args) => {
            out.push_str(name);
            out.push('(');
            for (i, a) in args.iter().enumerate() {
                if i > 0 {
                    out.push(',');
                }
                cg_expr(a, out);
            }
            out.push(')');
        }
    }
}

fn cg_stmts(stmts: &[Stmt], out: &mut String, indent: usize) {
    for s in stmts {
        for _ in 0..indent {
            out.push('\t');
        }
        match s {
            Stmt::Let(n, e) => {
                out.push_str(&format!("vlong {n} = "));
                cg_expr(e, out);
                out.push_str(";\n");
            }
            Stmt::Assign(n, e) => {
                out.push_str(&format!("{n} = "));
                cg_expr(e, out);
                out.push_str(";\n");
            }
            Stmt::Expr(e) => {
                cg_expr(e, out);
                out.push_str(";\n");
            }
            Stmt::Return(Some(e)) => {
                out.push_str("return ");
                cg_expr(e, out);
                out.push_str(";\n");
            }
            Stmt::Return(None) => out.push_str("return;\n"),
            Stmt::Print(e) => {
                out.push_str("print(\"%lld\\n\", (vlong)(");
                cg_expr(e, out);
                out.push_str("));\n");
            }
            Stmt::If(c, then, els) => {
                out.push_str("if (");
                cg_expr(c, out);
                out.push_str(") {\n");
                cg_stmts(then, out, indent + 1);
                for _ in 0..indent {
                    out.push('\t');
                }
                if els.is_empty() {
                    out.push_str("}\n");
                } else {
                    out.push_str("} else {\n");
                    cg_stmts(els, out, indent + 1);
                    for _ in 0..indent {
                        out.push('\t');
                    }
                    out.push_str("}\n");
                }
            }
            Stmt::While(c, body) => {
                out.push_str("while (");
                cg_expr(c, out);
                out.push_str(") {\n");
                cg_stmts(body, out, indent + 1);
                for _ in 0..indent {
                    out.push('\t');
                }
                out.push_str("}\n");
            }
        }
    }
}

fn codegen(fns: &[Func]) -> String {
    let mut out = String::new();
    out.push_str("#include <u.h>\n#include <libc.h>\n\n");
    // forward declarations (all fns are vlong except main)
    for f in fns {
        if f.is_main {
            continue;
        }
        out.push_str(&format!(
            "vlong {}({});\n",
            f.name,
            if f.params.is_empty() {
                "void".to_string()
            } else {
                f.params.iter().map(|_| "vlong").collect::<Vec<_>>().join(", ")
            }
        ));
    }
    out.push('\n');
    for f in fns {
        if f.is_main {
            out.push_str("void\nmain(void)\n{\n");
            cg_stmts(&f.body, &mut out, 1);
            out.push_str("\texits(nil);\n}\n\n");
        } else {
            let params = if f.params.is_empty() {
                "void".to_string()
            } else {
                f.params.iter().map(|p| format!("vlong {p}")).collect::<Vec<_>>().join(", ")
            };
            out.push_str(&format!("vlong\n{}({})\n{{\n", f.name, params));
            cg_stmts(&f.body, &mut out, 1);
            out.push_str("}\n\n");
        }
    }
    out
}

// ---------------------------------------------------------------- driver
fn main() {
    let args: Vec<String> = std::env::args().collect();
    if args.len() < 2 {
        eprintln!("usage: minirustc FILE.rs");
        std::process::exit(2);
    }
    let src = match std::fs::read_to_string(&args[1]) {
        Ok(s) => s,
        Err(e) => {
            eprintln!("minirustc: cannot read {}: {e}", args[1]);
            std::process::exit(1);
        }
    };

    let toks = lex(&src).unwrap_or_else(|e| {
        eprintln!("lex error: {e}");
        std::process::exit(1);
    });
    let mut parser = Parser { t: toks, p: 0 };
    let fns = parser.program().unwrap_or_else(|e| {
        eprintln!("parse error: {e}");
        std::process::exit(1);
    });
    let c = codegen(&fns);

    let cpath = "/tmp/minirustc_out.c";
    if let Err(e) = std::fs::write(cpath, &c) {
        eprintln!("minirustc: cannot write {cpath}: {e}");
        std::process::exit(1);
    }
    println!("minirustc: compiled {} fn(s) -> {cpath} ({} bytes of C)", fns.len(), c.len());

    // Drive the on-box kencc toolchain: 6c compiles, 6l links.
    let run = |prog: &str, args: &[&str]| -> bool {
        match Command::new(prog).args(args).current_dir("/tmp").status() {
            Ok(s) => s.success() || s.code() == Some(0),
            Err(e) => {
                eprintln!("minirustc: failed to run {prog}: {e}");
                false
            }
        }
    };
    println!("minirustc: 6c minirustc_out.c");
    if !run("/bin/6c", &["minirustc_out.c"]) {
        eprintln!("minirustc: 6c failed");
        std::process::exit(1);
    }
    println!("minirustc: 6l -o minirustc_out minirustc_out.6");
    if !run("/bin/6l", &["-o", "minirustc_out", "minirustc_out.6"]) {
        eprintln!("minirustc: 6l failed");
        std::process::exit(1);
    }
    println!("minirustc: built /tmp/minirustc_out -- running it:\n----");
    let _ = Command::new("/tmp/minirustc_out").status();
    println!("----\nminirustc: done.");
}
