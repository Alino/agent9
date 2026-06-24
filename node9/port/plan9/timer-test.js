console.log("start");
os.setTimeout(() => console.log("timer A fired (100ms)"), 100);
os.setTimeout(() => console.log("timer B fired (50ms)"), 50);
let n = 0;
let id = os.setInterval(() => {
  console.log("interval tick", ++n);
  if (n >= 3) os.clearInterval(id);
}, 30);
console.log("end of main script");
