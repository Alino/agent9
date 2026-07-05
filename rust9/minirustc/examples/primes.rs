// Nested loops, modulo, conditionals — compiled and run on 9front by minirustc.
fn gcd(a: i64, b: i64) -> i64 {
    let mut x = a;
    let mut y = b;
    while y != 0 {
        let t = y;
        y = x % y;
        x = t;
    }
    return x;
}

fn main() {
    println!("{}", gcd(1071, 462)); // 21

    // count primes below 50 (should be 15)
    let mut count = 0;
    let mut n = 2;
    while n < 50 {
        let mut d = 2;
        let mut isprime = 1;
        while d * d <= n {
            if n % d == 0 {
                isprime = 0;
            }
            d = d + 1;
        }
        if isprime == 1 {
            count = count + 1;
        }
        n = n + 1;
    }
    println!("{}", count);
}
