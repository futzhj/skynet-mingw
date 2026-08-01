local function check(name, fn)
    local ok, err = pcall(fn)
    assert(ok, name .. ": " .. tostring(err))
    print("ok " .. name)
end

check("lfs", function()
    local lfs = require "lfs"
    assert(type(lfs.currentdir()) == "string")
end)

check("zlib", function()
    local zlib = require "zlib"
    local input = string.rep("zlib ", 20)
    local compressed = zlib.deflate()(input, "finish")
    assert(zlib.inflate()(compressed, "finish") == input)
end)

check("lz4", function()
    local lz4 = require "lz4"
    local input = string.rep("lz4 ", 20)
    assert(lz4.decompress(lz4.compress(input)) == input)
end)

check("lpeg", function()
    local lpeg = require "lpeg"
    assert(lpeg.match(lpeg.P "ok" * -1, "ok") == 3)
end)

check("cmsgpack", function()
    local cmsgpack = require "cmsgpack"
    local value = cmsgpack.unpack(cmsgpack.pack({ answer = 42 }))
    assert(value.answer == 42)
end)

check("cjson", function()
    local cjson = require "cjson"
    local value = cjson.decode(cjson.encode({ answer = 42 }))
    assert(value.answer == 42)
end)

check("sproto", function()
    local sproto = require "sproto"
    assert(type(sproto.new) == "function")
end)

check("openssl", function()
    assert(type(require "openssl") == "table")
end)

check("iconv", function()
    local iconv = require "iconv"
    local converter = assert(iconv.new("utf-8", "utf-8"))
    assert(converter:iconv("hello") == "hello")
end)

check("lsqlite3", function()
    local sqlite3 = require "lsqlite3"
    local db = assert(sqlite3.open_memory())
    assert(db:exec("CREATE TABLE smoke (value INTEGER)") == sqlite3.OK)
    assert(db:close())
end)

check("rocksdb", function()
    local rocksdb = require "rocksdb"
    assert(type(rocksdb.options) == "function")
    local options = rocksdb.options({ create_if_missing = true })
    assert(options)
    options:destroy()
end)

check("lualeveldb", function()
    local leveldb = require "lualeveldb"
    assert(type(leveldb.options) == "function")
    local options = leveldb.options()
    assert(options)
end)
