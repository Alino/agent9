---
name: plan9-rc
description: Plan 9 rc shell idioms — what's different from bash, common patterns
---

# Plan 9 rc shell

rc is the Plan 9 shell. It's NOT bash. Common pitfalls:

## Syntax differences

- **No `$()` or backticks for command substitution.** Use `` `{ } `` instead:
  ```
  for(f in `{ls *.c}) echo $f
  ```
- **No `[[ ]]` or `[ ]` test.** Use `~` for matching:
  ```
  if(~ $#* 0) echo no args
  if(~ $arg yes) echo affirmative
  ```
- **No `&&` / `||`.** Use `;` and structured `if`:
  ```
  cmd1 && cmd2          # NO — won't work
  if(cmd1) cmd2         # YES
  ```
- **For-loops parenthesize the variable list:**
  ```
  for(i in 1 2 3) echo $i      # rc
  for i in 1 2 3; do …; done   # bash — won't work in rc
  ```

## Variable expansion

- `$*` is the argument LIST (not a string).
- `$"*` joins with spaces.
- `$#*` is the argument count.
- No `${var:-default}`. Use:
  ```
  if(~ $#var 0) var=default
  ```

## Quoting

- Single quotes `'…'` are LITERAL — no expansion at all.
- No double quotes. Use single quotes or just don't quote.
- Escape with `''` (two singles) to embed a literal single quote.

## Redirection

- `>[2=1]` redirects stderr to stdout (instead of bash's `2>&1`).
- `>[2]/dev/null` to discard stderr.
- `<{cmd}` for process substitution (yes, rc has this — useful):
  ```
  diff <{a | sort} <{b | sort}
  ```

## Tool helpers

- `which prog` — `whatis prog`
- `seq N M` — not built-in. Use awk or just enumerate.
- `xargs` — not built-in. Pipe through `while read line; do …; done`-equivalent:
  ```
  cmd | while(line=`{read}){ ... }
  ```
  Or simpler: just `cat file | …` with shell globbing.

## When in doubt

Read `intro(1)` and `rc(1)` man pages on the live system. The 9front
man pages are short and precise.
