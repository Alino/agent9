-- lsp-client.lua — neovim9 G5c gate: initialize handshake + hover round-trip
-- against lsp-server.lua spawned over stdio pipes.
-- Run via: nvim --clean --headless -l lsp-client.lua
local out = function(s) io.stdout:write(s .. '\n') io.stdout:flush() end

local id = vim.lsp.start({
  name = 'plan9test',
  cmd = { '/bin/nvim', '--clean', '--headless', '-l', '/tmp/lsp-server.lua' },
})
if not id then out('FAIL: vim.lsp.start returned nil') os.exit(1) end

local ok = vim.wait(15000, function()
  local c = vim.lsp.get_clients({ id = id })[1]
  return c ~= nil and c.initialized
end, 100)
if not ok then out('FAIL: initialize timeout') os.exit(1) end
out('INIT OK')

local client = vim.lsp.get_clients({ id = id })[1]
out('CAPS hoverProvider: ' .. tostring(client.server_capabilities.hoverProvider))

local result
client:request('textDocument/hover', {
  textDocument = { uri = 'file:///tmp/hello.c' },
  position = { line = 0, character = 0 },
}, function(err, res) result = res or { err = err } end)
vim.wait(15000, function() return result ~= nil end, 100)

if result and result.contents and result.contents.value then
  out('HOVER: ' .. result.contents.value)
else
  out('FAIL: hover ' .. vim.inspect(result))
  os.exit(1)
end

client:stop()
vim.wait(3000, function() return client:is_stopped() end, 100)
out('STOPPED: ' .. tostring(client:is_stopped()))
out('LSPGATE PASS')
