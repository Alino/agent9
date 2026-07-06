-- lsp-server.lua — minimal LSP server for the neovim9 G5c gate.
-- Run via: nvim --clean --headless -l lsp-server.lua  (speaks LSP over stdio)
local function read_message()
  local len
  while true do
    local line = io.read('*l')
    if line == nil then return nil end
    line = line:gsub('\r$', '')
    if line == '' then break end
    local n = line:match('^Content%-Length: (%d+)')
    if n then len = tonumber(n) end
  end
  if not len then return nil end
  local body = io.read(len)
  if body == nil then return nil end
  return vim.json.decode(body)
end

local function send(msg)
  local body = vim.json.encode(msg)
  io.stdout:write(('Content-Length: %d\r\n\r\n%s'):format(#body, body))
  io.stdout:flush()
end

while true do
  local msg = read_message()
  if msg == nil then break end
  if msg.method == 'initialize' then
    send({ jsonrpc = '2.0', id = msg.id, result = {
      capabilities = { hoverProvider = true },
      serverInfo = { name = 'plan9-lsp' },
    } })
  elseif msg.method == 'textDocument/hover' then
    send({ jsonrpc = '2.0', id = msg.id, result = {
      contents = { kind = 'plaintext', value = 'hover from plan9-lsp' },
    } })
  elseif msg.method == 'shutdown' then
    send({ jsonrpc = '2.0', id = msg.id, result = vim.NIL })
  elseif msg.method == 'exit' then
    os.exit(0)
  end
end
