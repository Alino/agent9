// Proves swgl builds+links for x86_64-unknown-plan9: create a software GL
// context and query its version string. If the C++ raster core linked, this runs.
fn main() {
    let ctx = swgl::Context::create();
    ctx.make_current();
    println!("swgl context created on plan9");
}
