console.log("hello from node9 qjs on 9front!");
console.log("scriptArgs:", JSON.stringify(scriptArgs));
console.log("Date.now() > 0:", Date.now() > 0);
console.log("typeof std:", typeof std, "| typeof os:", typeof os);
console.log("std.getenv('home'):", std.getenv("home"));

// file I/O via std (quickjs-libc -> APE stdio)
let f = std.open("/tmp/node9_hello.txt", "w");
f.puts("written by node9 qjs\nline two\n");
f.close();
let g = std.open("/tmp/node9_hello.txt", "r");
let text = g.readAsString();
g.close();
console.log("readback bytes:", text.length);
console.log("readback:", JSON.stringify(text));

// os module sanity
console.log("typeof os.open:", typeof os.open, "| typeof os.read:", typeof os.read);
let [cwd, err] = os.getcwd();
console.log("os.getcwd:", cwd, "err:", err);

console.log("DONE");
