// Promises + async/await + timers + microtask queue, all on the Plan 9 loop.
function delay(ms) { return new Promise(resolve => os.setTimeout(resolve, ms)); }

async function worker(name, ms, rounds) {
  for (let i = 1; i <= rounds; i++) {
    await delay(ms);
    console.log(`[${name}] round ${i} (after ${ms}ms)`);
  }
  return `${name} finished`;
}

console.log("sync: scheduling two concurrent async workers");
Promise.all([
  worker("A", 40, 3),
  worker("B", 60, 2),
]).then(results => {
  console.log("Promise.all resolved:", JSON.stringify(results));
});

// microtask ordering check
Promise.resolve().then(() => console.log("microtask ran before any timer"));
console.log("sync: main script end");
