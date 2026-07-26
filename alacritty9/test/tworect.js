// tworect — a frame that damages TWO separate regions must present BOTH.
//
// The presenter ships damage as sub-rects, and each present call flushes the GL
// buffer; only the first flush of a frame copies anything out, so shipping the
// rects one call at a time left every rect after the first carrying the
// PREVIOUS frame's pixels. On screen: a region that silently stops repainting —
// pi9 draws its transcript and its input box in one frame, so after Enter the
// transcript updated while the input box kept showing the prompt just sent.
//
// This driver updates a top line and a bottom line in the same frame (two
// distant rects, nothing in between), counting up. Run it twice, once with
// ALACRITTY9_FULLFRAME=1, and compare the two canvases: with the bug the
// damage-path capture keeps a stale counter in one of the two regions.
//
//   node tworect.js [rows] [iterations]
var ROWS = parseInt(process.argv[2] || '0', 10) || (process.stdout.rows || 45);
var N = parseInt(process.argv[3] || '25', 10);

function at(row, col, text) {
  process.stdout.write('\x1b[' + row + ';' + col + 'H\x1b[2K' + text);
}

var i = 0;
function step() {
  i++;
  at(1, 1, 'TOP    counter ' + i + ' ');
  at(ROWS - 1, 1, 'BOTTOM counter ' + i + ' ');
  if (i < N) {
    return setTimeout(step, 100);
  }
  at(Math.floor(ROWS / 2), 1, 'TWORECT-DONE top=' + i + ' bottom=' + i);
  setTimeout(function () { process.exit(0); }, 20000);   // hold for the capture
}
process.stdout.write('\x1b[2J');
step();
