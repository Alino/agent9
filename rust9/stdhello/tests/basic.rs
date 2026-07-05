// Minimal libtest reproducer: does the #[test] harness run at all on plan9?
#[test]
fn t_add() {
    assert_eq!(2 + 2, 4);
}

#[test]
fn t_vec() {
    let v = vec![1u32, 2, 3];
    assert_eq!(v.iter().sum::<u32>(), 6);
}

#[test]
fn t_string() {
    let s = format!("{}-{}", "a", 42);
    assert_eq!(s, "a-42");
}
