// A real Rust program in the subset minirustc supports — compiled on 9front.
fn fib(n: i64) -> i64 {
    if n < 2 {
        return n;
    }
    return fib(n - 1) + fib(n - 2);
}

fn fact(n: i64) -> i64 {
    let mut acc = 1;
    let mut i = 2;
    while i <= n {
        acc = acc * i;
        i = i + 1;
    }
    return acc;
}

fn main() {
    let mut i = 0;
    while i <= 12 {
        println!("{}", fib(i));
        i = i + 1;
    }
    println!("{}", fact(10));
}
