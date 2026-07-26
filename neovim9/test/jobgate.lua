-- A job whose exec FAILS in the child must not take nvim's own threads with it.
-- cc9's fork() hands the child a copy of the worker-thread registry, whose pids
-- belong to nvim; a child that exits without exec'ing used to post "kill" to all
-- of them. nvim checks executable() before spawning, so the reachable version of
-- this is a file that IS executable by mode but cannot be exec'd: no #! line and
-- not an a.out. Then check a REAL job still delivers output — before the fix the
-- second jobwait comes back with nothing.
-- Run on 9front:  nvim --headless --clean -l test/jobgate.lua
local bad = '/tmp/nvim9-notabinary'
local f = io.open(bad, 'w'); f:write('this is not a binary\n'); f:close()
os.execute('chmod +x ' .. bad)
local dead = vim.fn.jobstart({ bad })
vim.wait(400)
local out = {}
local j = vim.fn.jobstart({ 'cat', '/env/user' }, {
  on_stdout = function(_, data)
    for _, l in ipairs(data) do if l ~= '' then table.insert(out, l) end end
  end,
})
local rc = vim.fn.jobwait({ j }, 5000)[1]
io.write(('failed_spawn=%d live_job_rc=%d out=%q\n'):format(dead, rc, table.concat(out, ',')))
io.write((rc == 0 and #out > 0) and 'JOBGATE PASS\n' or 'JOBGATE FAIL\n')
