// Phase 2 exit test: a timer fires AND an async fd read callback runs on the loop.
console.log("start: async fd I/O via os.setReadHandler + os.pipe");
let [rd, wr] = os.pipe();
console.log("pipe fds:", rd, wr);
let got = "";
os.setReadHandler(rd, () => {
  let buf = new Uint8Array(64);
  let n = os.read(rd, buf.buffer, 0, 64);
  if (n > 0) {
    for (let i = 0; i < n; i++) got += String.fromCharCode(buf[i]);
    console.log("read handler fired: +" + n + " bytes -> " + JSON.stringify(got));
  }
  if (n <= 0 || got.length >= 5) {
    os.setReadHandler(rd, null);
    os.close(rd);
    console.log("FINAL: " + JSON.stringify(got));
  }
});
// write later, from a timer, so the loop is genuinely multiplexing
os.setTimeout(() => {
  let data = new Uint8Array([72, 69, 76, 76, 79]); // "HELLO"
  os.write(wr, data.buffer, 0, 5);
  os.close(wr);
  console.log("timer fired: wrote HELLO to pipe");
}, 50);
console.log("main script done; event loop takes over");
